# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""Ferramenta de bancada (proposta §4.8): lê a USB do ESP32 no notebook, confere cada linha do
protocolo (proposta §5) e mostra o resumo.

    uv run ferramentas/bancada.py                  mostra as linhas até o Ctrl+C
    uv run ferramentas/bancada.py --segundos 300   captura 5 min e sai com erro se houver problema
    uv run ferramentas/bancada.py --porta /dev/ttyUSB0 --reiniciar

Sem o uv: `pip install pyserial==3.5` e `python ferramentas/bancada.py`.

Sem --porta, procura a placa pelo chip USB (CP210x ou CH340/CH9102). Com --segundos, só mostra
os problemas e as linhas raras ($B, $L, $P); o código de saída é 1 se houve linha corrompida,
lacuna na seq ou nenhuma linha válida.

O que conta como problema:
- corrompida: mais de 120 caracteres (sem o \\r\\n), sem $, sem *hh, com checksum errado ou sem
  a seq no primeiro campo;
- perdida: cada número que falta na seq;
- fora de ordem: a seq andou para trás sem voltar a 0.

A seq voltar a 0 é reinício do ESP32, não lacuna. O texto que a ROM imprime no boot, e a linha
cortada pelo reinício, chegam antes do $B com seq 0 e são ignorados sem contar como corrompidos.
O mesmo vale para o que chega antes da primeira linha válida (a ferramenta pode abrir a porta no
meio de uma linha).
"""
import argparse
import dataclasses
import sys
import time

BAUD = 115200
TAMANHO_MAXIMO = 120
# VID dos chips USB das placas DevKit: CP210x (Silicon Labs) e CH340/CH9102 (WCH).
VIDS_DA_PLACA = {0x10C4: "CP210x", 0x1A86: "WCH"}
# Linhas raras, que aparecem também no modo com tempo.
TIPOS_RAROS = {"B", "L", "P"}


def checksum(texto: bytes) -> int:
    """O XOR de todos os bytes: o checksum do NMEA."""
    resultado = 0
    for byte in texto:
        resultado ^= byte
    return resultado


@dataclasses.dataclass
class Valida:
    tipo: str
    seq: int
    campos: list[str]


def ler_linha(bruta: bytes) -> Valida | str:
    """Confere a moldura $<tipo>,<campos...>*<hh>. Devolve a linha ou o motivo da recusa."""
    linha = bruta.rstrip(b"\r")
    if len(linha) > TAMANHO_MAXIMO:
        return f"mais de {TAMANHO_MAXIMO} caracteres"
    if not linha.startswith(b"$"):
        return "sem $"
    asterisco = linha.rfind(b"*")
    if asterisco < 0 or len(linha) - asterisco != 3:
        return "sem *hh"
    hh = linha[asterisco + 1:]
    if not all(c in b"0123456789ABCDEF" for c in hh):
        return "sem *hh"
    corpo = linha[1:asterisco]
    if checksum(corpo) != int(hh, 16):
        return f"checksum errado (veio {hh.decode()}, a conta dá {checksum(corpo):02X})"
    campos = corpo.decode("ascii", errors="replace").split(",")
    if len(campos) < 2 or not campos[1].isdigit():
        return "sem a seq"
    return Valida(tipo=campos[0], seq=int(campos[1]), campos=campos)


class Conferencia:
    """Conta as linhas do ESP32 e acha os problemas. Não sabe nada da porta: recebe as linhas
    já separadas pelo \\n, e devolve o que mostrar."""

    def __init__(self):
        self.validas = 0
        self.corrompidas = 0
        self.perdidas = 0
        self.fora_de_ordem = 0
        self.reinicios = 0
        self.ignoradas = 0
        self.por_tipo: dict[str, int] = {}
        self.saltos_uptime = 0
        self._ultima_seq: int | None = None
        self._ultimo_uptime: int | None = None
        # Linhas recusadas esperando a próxima válida: se ela for um reinício (seq 0), eram o
        # boot; se não, eram corrompidas.
        self._pendentes: list[tuple[bytes, str]] = []

    def linha(self, bruta: bytes) -> list[str]:
        resultado = ler_linha(bruta)
        if isinstance(resultado, str):
            self._pendentes.append((bruta, resultado))
            return []
        return self._valida(resultado, bruta)

    def terminar(self) -> list[str]:
        """No fim da captura, as recusadas sem uma válida depois contam como corrompidas, a não
        ser que nenhuma linha válida tenha chegado (nada para comparar)."""
        if self._ultima_seq is None:
            return self._ignorar_pendentes()
        return self._corromper_pendentes()

    def resumo(self) -> str:
        tipos = ", ".join(f"${tipo} {n}" for tipo, n in sorted(self.por_tipo.items()))
        return "\n".join([
            f"  válidas: {self.validas}" + (f" ({tipos})" if tipos else ""),
            f"  corrompidas: {self.corrompidas}",
            f"  perdidas (lacunas na seq): {self.perdidas}",
            f"  fora de ordem: {self.fora_de_ordem}",
            f"  reinícios do ESP32: {self.reinicios}",
            f"  texto fora do protocolo (boot): {self.ignoradas}",
            f"  saltos no uptime_s do $S: {self.saltos_uptime}",
        ])

    def falhou(self) -> bool:
        return (self.corrompidas > 0 or self.perdidas > 0 or self.fora_de_ordem > 0
                or self.validas == 0)

    def _valida(self, linha: Valida, bruta: bytes) -> list[str]:
        mensagens = []
        reinicio = linha.seq == 0 and self._ultima_seq is not None and self._ultima_seq != 0xFFFFFFFF
        if self._ultima_seq is None or reinicio:
            mensagens += self._ignorar_pendentes()
        else:
            mensagens += self._corromper_pendentes()

        if reinicio:
            self.reinicios += 1
            self._ultimo_uptime = None
            mensagens.append("-- reinício do ESP32: a seq voltou a 0")
        elif self._ultima_seq is not None:
            esperada = (self._ultima_seq + 1) & 0xFFFFFFFF
            if linha.seq > esperada:
                faltam = linha.seq - esperada
                self.perdidas += faltam
                mensagens.append(f"!! lacuna na seq: {faltam} linha(s) perdida(s), "
                                 f"de {self._ultima_seq} para {linha.seq}")
            elif linha.seq < esperada:
                self.fora_de_ordem += 1
                mensagens.append(f"!! seq fora de ordem: de {self._ultima_seq} para {linha.seq}")
        self._ultima_seq = linha.seq

        self.validas += 1
        self.por_tipo[linha.tipo] = self.por_tipo.get(linha.tipo, 0) + 1
        if linha.tipo == "S" and len(linha.campos) > 3 and linha.campos[3].isdigit():
            uptime = int(linha.campos[3])
            if self._ultimo_uptime is not None and uptime != self._ultimo_uptime + 1:
                self.saltos_uptime += 1
                mensagens.append(f"!! uptime_s saltou de {self._ultimo_uptime} para {uptime}")
            self._ultimo_uptime = uptime
        mensagens.append(bruta.decode("ascii", errors="replace").rstrip("\r"))
        return mensagens

    def _ignorar_pendentes(self) -> list[str]:
        mensagens = [f"   (fora do protocolo) {mostrar(bruta)}" for bruta, _ in self._pendentes]
        self.ignoradas += len(self._pendentes)
        self._pendentes.clear()
        return mensagens

    def _corromper_pendentes(self) -> list[str]:
        mensagens = [f"!! corrompida ({motivo}): {mostrar(bruta)}"
                     for bruta, motivo in self._pendentes]
        self.corrompidas += len(self._pendentes)
        self._pendentes.clear()
        return mensagens


def mostrar(bruta: bytes) -> str:
    return bruta.decode("ascii", errors="backslashreplace").rstrip("\r")


def rara_ou_problema(mensagem: str) -> bool:
    """No modo com tempo, só aparecem os problemas, os reinícios e as linhas raras."""
    if mensagem.startswith(("!!", "--")):
        return True
    return mensagem.startswith("$") and mensagem[1:2] in TIPOS_RAROS


def achar_porta() -> str:
    from serial.tools import list_ports

    placas = [p for p in list_ports.comports() if p.vid in VIDS_DA_PLACA]
    if len(placas) == 1:
        return placas[0].device
    if not placas:
        sys.exit("Nenhuma placa achada pelo chip USB (CP210x ou WCH). Use --porta.")
    lista = ", ".join(f"{p.device} ({VIDS_DA_PLACA[p.vid]})" for p in placas)
    sys.exit(f"Mais de uma placa: {lista}. Escolha com --porta.")


def reiniciar_pelo_en(porta) -> None:
    """Pulsa o EN pelo RTS, como o esptool: o circuito da DevKit reinicia o ESP32."""
    porta.dtr = False
    porta.rts = True
    time.sleep(0.1)
    porta.rts = False


def main() -> int:
    argumentos = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    argumentos.add_argument("--porta", help="porta USB do ESP32 (sem ela, procura pelo chip)")
    argumentos.add_argument("--segundos", type=float,
                            help="captura por N segundos, mostra o resumo e sai")
    argumentos.add_argument("--reiniciar", action="store_true",
                            help="reinicia o ESP32 pelo EN ao abrir a porta, para pegar o $B")
    args = argumentos.parse_args()

    import serial

    nome_porta = args.porta or achar_porta()
    porta = serial.Serial(nome_porta, BAUD, bytesize=serial.EIGHTBITS, parity=serial.PARITY_NONE,
                          stopbits=serial.STOPBITS_ONE, xonxoff=False, rtscts=False,
                          dsrdtr=False, timeout=0.1)
    if args.reiniciar:
        reiniciar_pelo_en(porta)
    print(f"Lendo {nome_porta} a {BAUD} baud" +
          (f" por {args.segundos:g} s" if args.segundos else ", Ctrl+C para sair") + ".")

    conferencia = Conferencia()
    resto = b""
    inicio = time.monotonic()
    try:
        while args.segundos is None or time.monotonic() - inicio < args.segundos:
            resto += porta.read(porta.in_waiting or 1)
            *linhas, resto = resto.split(b"\n")
            for bruta in linhas:
                for mensagem in conferencia.linha(bruta):
                    if args.segundos is None or rara_ou_problema(mensagem):
                        print(f"{time.monotonic() - inicio:9.3f}  {mensagem}")
    except KeyboardInterrupt:
        pass
    finally:
        porta.close()
    # O pedaço sem \n no fim foi cortado pela própria captura e não conta.
    for mensagem in conferencia.terminar():
        if args.segundos is None or rara_ou_problema(mensagem):
            print(f"{time.monotonic() - inicio:9.3f}  {mensagem}")

    print(f"\nResumo de {time.monotonic() - inicio:.1f} s em {nome_porta}:")
    print(conferencia.resumo())
    return 1 if conferencia.falhou() else 0


if __name__ == "__main__":
    sys.exit(main())
