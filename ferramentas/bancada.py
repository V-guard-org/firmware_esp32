# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial==3.5"]
# ///
"""Ferramenta de bancada (proposta §4.8): lê a USB do ESP32 no notebook, confere cada linha do
protocolo (proposta §5), faz o papel da Pi e mostra o resumo.

    uv run ferramentas/bancada.py                  mostra as linhas até o Ctrl+C
    uv run ferramentas/bancada.py --segundos 300   captura 5 min e sai com erro se houver problema
    uv run ferramentas/bancada.py --porta /dev/ttyUSB0 --reiniciar
    uv run ferramentas/bancada.py --segundos 8 --enviar C,obd_hz,15 --enviar C,obd_hz,25
    uv run ferramentas/bancada.py --antes-da-virada 10 --salvar captura.txt

Sem o uv: `pip install pyserial==3.5` e `python ferramentas/bancada.py`.

Sem --porta, procura a placa pelo chip USB (CP210x ou CH340/CH9102). Com --segundos, só mostra
os problemas, o que foi mandado ao ESP32 e as linhas raras ($B, $L, $P); o código de saída é 1
se houve algum dos problemas abaixo ou nenhuma linha válida.

O papel da Pi (proposta §5.5):
- manda o $H com a hora do notebook ao ver um $B ou um $S com ESPERA, e depois a cada 60 s. A
  primeira linha válida também dispara um, para o relógio do ESP32 que já estava em ATIVO.
  --sem-hora não manda, para ver o ESP32 em ESPERA;
- o que se digita no terminal, seguido de Enter, vai ao ESP32 com a moldura: C,obd_hz,15 sai
  como $C,obd_hz,15*hh. A linha que começa com $ vai como foi digitada;
- --enviar <linha>, repetível, manda cada linha com 2 s de intervalo, a partir da primeira linha
  válida;
- --antes-da-virada S: a hora que a ferramenta manda começa S segundos antes da próxima hora
  cheia (UTC) e anda junto com a do notebook. Os $H seguintes, também depois de um reinício, e a
  conferência do t usam essa hora deslocada, nunca a hora real do notebook. Serve para ver a
  troca de arquivo do SD na virada da hora.

--salvar ARQ guarda as linhas válidas como chegaram e os $H mandados, uma por linha, para
comparar com o cartão: python ferramentas/comparar_sd.py ARQ /media/.../*.csv

O que conta como problema:
- corrompida: mais de 120 caracteres (sem o \\r\\n), sem $, sem *hh, com checksum errado ou sem
  a seq no primeiro campo;
- perdida: cada número que falta na seq;
- fora de ordem: a seq andou para trás sem voltar a 0;
- fora da hora: o t da linha a mais de 1 s da hora do notebook na chegada (o t 0, sem hora, não
  conta);
- continua em ESPERA: um $S com ESPERA mais de 3 s depois de um $H mandado pela ferramenta;
- taxa do $A: entre dois $S seguidos em ATIVO, menos de 49 ou mais de 51 $A. --sem-imu não
  confere, para as bancadas sem o ADXL345 ligado;
- descartadas: o campo descartadas do $S passou de 0 (linhas que não couberam na saída do
  ESP32).

Também aparecem, no modo com tempo, a mudança do imu, do obd e do sd do $S e a pausa e a volta
do $A (um intervalo entre dois $S em ATIVO sem nenhum $A). O resumo traz as médias de ax, ay, az
e |a|.

A seq voltar a 0 é reinício do ESP32, não lacuna. O texto que a ROM imprime no boot, e a linha
cortada pelo reinício, chegam antes do $B com seq 0 e são ignorados sem contar como corrompidos.
O mesmo vale para o que chega antes da primeira linha válida (a ferramenta pode abrir a porta no
meio de uma linha).
"""
import argparse
import dataclasses
import queue
import sys
import threading
import time

BAUD = 115200
TAMANHO_MAXIMO = 120
# VID dos chips USB das placas DevKit: CP210x (Silicon Labs) e CH340/CH9102 (WCH).
VIDS_DA_PLACA = {0x10C4: "CP210x", 0x1A86: "WCH"}
# Linhas raras, que aparecem também no modo com tempo.
TIPOS_RAROS = {"B", "L", "P"}
# O t pode estar até 1 s da hora do notebook (critério do #3).
DISTANCIA_MAXIMA_MS = 1000
# Depois de um $H, o ESP32 tem até 3 s para o $S sair de ESPERA.
PRAZO_PARA_ATIVO_MS = 3000
# Entre dois $S em ATIVO (1 s), 50 $A, com 1 de folga pela fase entre as duas tarefas (#4).
A_POR_STATUS = range(49, 52)
# Campos do $S: $S,<seq>,<t>,<uptime_s>,<estado>,<obd>,<imu>,<sd>,<sd_livre>,<obd_hz>,<descartadas>
CAMPOS_DO_STATUS = 11
# Campos do $S cuja mudança aparece na tela, pela posição em campos (o tipo não conta).
MUDANCAS_DO_STATUS = {"obd": 5, "sd": 7}
MS_POR_HORA = 3_600_000


def checksum(texto: bytes) -> int:
    """O XOR de todos os bytes: o checksum do NMEA."""
    resultado = 0
    for byte in texto:
        resultado ^= byte
    return resultado


def enmoldurar(texto: str) -> bytes:
    """Põe a moldura numa linha para o ESP32: "C,obd_hz,15" vira b"$C,obd_hz,15*63\\n". A que já
    começa com $ vai como está, para testar uma moldura errada."""
    texto = texto.strip()
    if texto.startswith("$"):
        return texto.encode() + b"\n"
    corpo = texto.encode()
    return b"$" + corpo + f"*{checksum(corpo):02X}\n".encode()


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

    def __init__(self, conferir_taxa_do_a: bool = True):
        # Sem o ADXL345 ligado (--sem-imu), não chega $A, e a taxa não é conferida.
        self.conferir_taxa_do_a = conferir_taxa_do_a
        self.validas = 0
        self.corrompidas = 0
        self.perdidas = 0
        self.fora_de_ordem = 0
        self.reinicios = 0
        self.ignoradas = 0
        self.por_tipo: dict[str, int] = {}
        self.saltos_uptime = 0
        self.horas_enviadas = 0
        self.fora_da_hora = 0
        self.esperas = 0
        # Intervalos entre dois $S em ATIVO com o $A fora de 49 a 51, e o menor e o maior.
        self.fora_da_taxa = 0
        self.menor_a_por_status: int | None = None
        self.maior_a_por_status: int | None = None
        # Linhas que o ESP32 descartou durante a captura, pelo campo descartadas do $S.
        self.descartadas = 0
        # Somas do $A, para as médias: ax, ay, az e |a|.
        self.quantidade_a = 0
        self._somas_a = [0.0, 0.0, 0.0, 0.0]
        # Maior distância entre o t e a hora do notebook, em ms, com o sinal.
        self.maior_distancia_ms: int | None = None
        # A última linha válida, ou None se a última foi recusada.
        self.ultima: Valida | None = None
        self._ultima_seq: int | None = None
        self._fora_da_hora_agora = False
        self._hora_enviada_em: int | None = None
        self._avisou_espera = False
        self._ultimo_uptime: int | None = None
        self._a_desde_o_status = 0
        self._ultimo_estado: str | None = None
        self._ultimo_imu: str | None = None
        self._ultimos_do_status: dict[str, str] = {}
        self._ultimas_descartadas = 0
        self._a_parado = False
        # Linhas recusadas esperando a próxima válida: se ela for um reinício (seq 0), eram o
        # boot; se não, eram corrompidas.
        self._pendentes: list[tuple[bytes, str]] = []

    def linha(self, bruta: bytes, hora_ms: int | None = None) -> list[str]:
        """Confere uma linha. Com hora_ms (a hora do notebook na chegada), confere também o t e a
        saída de ESPERA."""
        resultado = ler_linha(bruta)
        if isinstance(resultado, str):
            self.ultima = None
            self._pendentes.append((bruta, resultado))
            return []
        self.ultima = resultado
        return self._valida(resultado, bruta, hora_ms)

    def hora_enviada(self, hora_ms: int) -> None:
        """A ferramenta mandou um $H: o ESP32 tem 3 s para o $S sair de ESPERA."""
        self.horas_enviadas += 1
        self._hora_enviada_em = hora_ms
        self._avisou_espera = False

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
            f"  $H enviados: {self.horas_enviadas}",
            "  maior distância entre o t e o notebook: " + (
                "nenhum t para comparar" if self.maior_distancia_ms is None
                else f"{self.maior_distancia_ms:+d} ms"),
            f"  linhas com o t a mais de 1 s do notebook: {self.fora_da_hora}",
            f"  $S em ESPERA mais de 3 s depois do $H: {self.esperas}",
            "  $A entre dois $S em ATIVO: " + (
                "não conferido (--sem-imu)" if not self.conferir_taxa_do_a
                else "nenhum intervalo para comparar" if self.menor_a_por_status is None
                else f"de {self.menor_a_por_status} a {self.maior_a_por_status}"
                     f" (fora de 49 a 51: {self.fora_da_taxa})"),
            f"  descartadas no ESP32: {self.descartadas}",
            "  médias do $A: " + self._medias_a(),
        ])

    def _medias_a(self) -> str:
        if self.quantidade_a == 0:
            return "nenhum $A"
        ax, ay, az, modulo = (soma / self.quantidade_a for soma in self._somas_a)
        return (f"ax {ax:+.2f}, ay {ay:+.2f}, az {az:+.2f}, |a| {modulo:.2f} m/s²"
                f" ({self.quantidade_a} linhas)")

    def falhou(self) -> bool:
        return (self.corrompidas > 0 or self.perdidas > 0 or self.fora_de_ordem > 0
                or self.fora_da_hora > 0 or self.esperas > 0 or self.fora_da_taxa > 0
                or self.descartadas > 0 or self.validas == 0)

    def _valida(self, linha: Valida, bruta: bytes, hora_ms: int | None) -> list[str]:
        mensagens = []
        reinicio = linha.seq == 0 and self._ultima_seq is not None and self._ultima_seq != 0xFFFFFFFF
        if self._ultima_seq is None or reinicio:
            mensagens += self._ignorar_pendentes()
        else:
            mensagens += self._corromper_pendentes()

        if reinicio or linha.tipo == "B":
            # O ESP32 voltou a ESPERA; o $H mandado antes não vale mais.
            self._hora_enviada_em = None
        if reinicio:
            self.reinicios += 1
            self._ultimo_uptime = None
            self._ultimo_estado = None
            self._ultimas_descartadas = 0
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
        if linha.tipo == "A":
            mensagens += self._acelerometro(linha)
        elif linha.tipo == "S" and len(linha.campos) == CAMPOS_DO_STATUS:
            mensagens += self._mudancas_do_status(linha)
            mensagens += self._status_do_acelerometro(linha)
        if hora_ms is not None:
            mensagens += self._conferir_hora(linha, hora_ms)
        mensagens.append(bruta.decode("ascii", errors="replace").rstrip("\r"))
        return mensagens

    def _conferir_hora(self, linha: Valida, hora_ms: int) -> list[str]:
        mensagens = []
        if len(linha.campos) > 2 and linha.campos[2].isdigit() and linha.campos[2] != "0":
            distancia = int(linha.campos[2]) - hora_ms
            if self.maior_distancia_ms is None or abs(distancia) > abs(self.maior_distancia_ms):
                self.maior_distancia_ms = distancia
            fora = abs(distancia) > DISTANCIA_MAXIMA_MS
            if fora:
                self.fora_da_hora += 1
                if not self._fora_da_hora_agora:
                    mensagens.append(f"!! t a {distancia:+d} ms da hora do notebook")
            self._fora_da_hora_agora = fora
        espera = linha.tipo == "S" and len(linha.campos) > 4 and linha.campos[4] == "ESPERA"
        if (espera and self._hora_enviada_em is not None and not self._avisou_espera
                and hora_ms - self._hora_enviada_em > PRAZO_PARA_ATIVO_MS):
            self.esperas += 1
            self._avisou_espera = True
            mensagens.append("!! continua em ESPERA mais de 3 s depois do $H")
        return mensagens


    def _acelerometro(self, linha: Valida) -> list[str]:
        """$A,<seq>,<t>,<ax>,<ay>,<az>: conta para a taxa e soma para as médias."""
        mensagens = []
        self._a_desde_o_status += 1
        if self._a_parado:
            self._a_parado = False
            mensagens.append("-- o $A voltou")
        try:
            ax, ay, az = (float(campo) for campo in linha.campos[3:6])
        except ValueError:
            return mensagens
        if len(linha.campos) == 6:
            self.quantidade_a += 1
            for i, valor in enumerate((ax, ay, az, (ax * ax + ay * ay + az * az) ** 0.5)):
                self._somas_a[i] += valor
        return mensagens

    def _mudancas_do_status(self, linha: Valida) -> list[str]:
        """No $S: a mudança do obd (LENDO → SEM_RESPOSTA etc.), para os cenários do simulador, e
        a do sd (OK → FALHA)."""
        mensagens = []
        for nome, posicao in MUDANCAS_DO_STATUS.items():
            valor = linha.campos[posicao]
            anterior = self._ultimos_do_status.get(nome)
            if valor != anterior:
                mensagens.append(f"-- {nome}: {valor}" if anterior is None
                                 else f"-- {nome}: {anterior} → {valor}")
                self._ultimos_do_status[nome] = valor
        return mensagens

    def _status_do_acelerometro(self, linha: Valida) -> list[str]:
        """No $S: a mudança do imu, as descartadas e a quantidade de $A desde o $S anterior."""
        mensagens = []
        estado, imu, descartadas = linha.campos[4], linha.campos[6], linha.campos[10]
        if imu != self._ultimo_imu:
            mensagens.append(f"-- imu: {imu}" if self._ultimo_imu is None
                             else f"-- imu: {self._ultimo_imu} → {imu}")
            self._ultimo_imu = imu
        if descartadas.isdigit():
            if int(descartadas) > self._ultimas_descartadas:
                self.descartadas += int(descartadas) - self._ultimas_descartadas
                mensagens.append(f"!! o ESP32 descartou linhas: descartadas {descartadas}")
            self._ultimas_descartadas = int(descartadas)
        if self.conferir_taxa_do_a and self._ultimo_estado == "ATIVO" and estado == "ATIVO":
            mensagens += self._taxa_do_acelerometro(self._a_desde_o_status)
        self._ultimo_estado = estado
        self._a_desde_o_status = 0
        return mensagens

    def _taxa_do_acelerometro(self, quantidade: int) -> list[str]:
        if self.menor_a_por_status is None or quantidade < self.menor_a_por_status:
            self.menor_a_por_status = quantidade
        if self.maior_a_por_status is None or quantidade > self.maior_a_por_status:
            self.maior_a_por_status = quantidade
        if quantidade in A_POR_STATUS:
            return []
        self.fora_da_taxa += 1
        if quantidade > 0:
            return [f"!! {quantidade} $A entre dois $S (o certo é de 49 a 51)"]
        # Parado, avisa uma vez só, e não a cada segundo.
        if self._a_parado:
            return []
        self._a_parado = True
        return ["-- o $A parou: nenhum $A entre dois $S"]

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


class PapelDaPi:
    """Faz o papel da Pi (proposta §5.5): decide quando mandar o $H e as linhas do --enviar. Não
    sabe nada da porta: recebe as linhas válidas e o tempo, e devolve o que mandar."""

    INTERVALO_HORA = 60.0
    # Depois de um $H, um $S com ESPERA só dispara outro passado este tempo: o $S que já estava
    # a caminho não deve repetir o $H.
    PAUSA_ENTRE_HORAS = 2.0
    INTERVALO_ENVIAR = 2.0

    def __init__(self, mandar_hora: bool = True, enviar: list[str] | tuple[str, ...] = ()):
        self.mandar_hora = mandar_hora
        self._enviar = list(enviar)
        self._agenda: list[tuple[float, str]] = []
        self._ultima_hora: float | None = None
        self._viu_valida = False

    def recebeu(self, linha: Valida, agora: float, hora_ms: int) -> list[bytes]:
        """agora: o tempo monotônico, em s; hora_ms: a hora do notebook, em ms."""
        if not self._viu_valida:
            self._viu_valida = True
            self._agenda = [(agora + self.INTERVALO_ENVIAR * (i + 1), texto)
                            for i, texto in enumerate(self._enviar)]
        if not self.mandar_hora:
            return []
        espera = linha.tipo == "S" and len(linha.campos) > 4 and linha.campos[4] == "ESPERA"
        pausa_passou = (self._ultima_hora is None
                        or agora - self._ultima_hora >= self.PAUSA_ENTRE_HORAS)
        if linha.tipo == "B" or self._ultima_hora is None or (espera and pausa_passou):
            return [self._hora(agora, hora_ms)]
        return []

    def tique(self, agora: float, hora_ms: int) -> list[bytes]:
        """O $H de 60 em 60 s."""
        if (self.mandar_hora and self._ultima_hora is not None
                and agora - self._ultima_hora >= self.INTERVALO_HORA):
            return [self._hora(agora, hora_ms)]
        return []

    def agendadas(self, agora: float) -> list[bytes]:
        """As linhas do --enviar que chegaram na hora. São pedidas por quem usa e podem ser
        inválidas de propósito, por isso ficam à parte do $H da ferramenta."""
        envios = []
        while self._agenda and self._agenda[0][0] <= agora:
            envios.append(enmoldurar(self._agenda.pop(0)[1]))
        return envios

    def _hora(self, agora: float, hora_ms: int) -> bytes:
        self._ultima_hora = agora
        return enmoldurar(f"H,{hora_ms}")


def deslocamento_antes_da_virada(agora_ms: int, segundos: float) -> int:
    """Quanto somar à hora do notebook para ela marcar `segundos` antes da próxima hora cheia
    (UTC), em ms."""
    proxima_hora = (agora_ms // MS_POR_HORA + 1) * MS_POR_HORA
    return proxima_hora - round(segundos * 1000) - agora_ms


def mostrar(bruta: bytes) -> str:
    return bruta.decode("ascii", errors="backslashreplace").rstrip("\r")


def rara_ou_problema(mensagem: str) -> bool:
    """No modo com tempo, só aparecem os problemas, os reinícios, o que foi mandado e as linhas
    raras."""
    if mensagem.startswith(("!!", "--", ">>")):
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


def ler_teclado(fila: queue.Queue) -> None:
    """Põe na fila cada linha digitada no terminal."""
    for texto in sys.stdin:
        if texto.strip():
            fila.put(texto)


def main() -> int:
    argumentos = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    argumentos.add_argument("--porta", help="porta USB do ESP32 (sem ela, procura pelo chip)")
    argumentos.add_argument("--segundos", type=float,
                            help="captura por N segundos, mostra o resumo e sai")
    argumentos.add_argument("--reiniciar", action="store_true",
                            help="reinicia o ESP32 pelo EN ao abrir a porta, para pegar o $B")
    argumentos.add_argument("--sem-hora", action="store_true",
                            help="não manda o $H, para ver o ESP32 em ESPERA")
    argumentos.add_argument("--sem-imu", action="store_true",
                            help="não confere a taxa do $A, para a bancada sem o ADXL345 ligado")
    argumentos.add_argument("--enviar", action="append", default=[], metavar="LINHA",
                            help="manda a linha (C,obd_hz,15) com a moldura; repetível, com 2 s "
                                 "de intervalo a partir da primeira linha válida")
    argumentos.add_argument("--antes-da-virada", type=float, metavar="S",
                            help="a hora mandada começa S segundos antes da próxima hora cheia "
                                 "(UTC), para ver a troca de arquivo do SD")
    argumentos.add_argument("--salvar", metavar="ARQ",
                            help="guarda as linhas válidas e os $H mandados, para comparar com o "
                                 "cartão (comparar_sd.py)")
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

    deslocamento_ms = 0
    if args.antes_da_virada is not None:
        deslocamento_ms = deslocamento_antes_da_virada(int(time.time() * 1000),
                                                       args.antes_da_virada)
        print(f"Hora deslocada em {deslocamento_ms / 1000:+.3f} s: a virada da hora vem em "
              f"{args.antes_da_virada:g} s.")
    salvar = open(args.salvar, "w", encoding="ascii", newline="\n") if args.salvar else None

    conferencia = Conferencia(conferir_taxa_do_a=not args.sem_imu)
    pi = PapelDaPi(mandar_hora=not args.sem_hora, enviar=args.enviar)
    teclado: queue.Queue = queue.Queue()
    threading.Thread(target=ler_teclado, args=(teclado,), daemon=True).start()
    resto = b""
    inicio = time.monotonic()

    def mostrar_mensagens(mensagens: list[str]) -> None:
        for mensagem in mensagens:
            if args.segundos is None or rara_ou_problema(mensagem):
                print(f"{time.monotonic() - inicio:9.3f}  {mensagem}")

    def mandar(envios: list[bytes], hora_ms: int, da_ferramenta: bool = True) -> None:
        for envio in envios:
            porta.write(envio)
            # Só o $H da própria ferramenta conta no prazo para sair de ESPERA: um digitado ou
            # do --enviar pode ser inválido de propósito.
            if da_ferramenta and envio.startswith(b"$H,"):
                conferencia.hora_enviada(hora_ms)
            if salvar is not None and envio.startswith(b"$H,"):
                salvar.write(envio.decode(errors="backslashreplace").rstrip() + "\n")
            mostrar_mensagens([">> " + envio.decode(errors="backslashreplace").rstrip()])

    try:
        while args.segundos is None or time.monotonic() - inicio < args.segundos:
            resto += porta.read(porta.in_waiting or 1)
            *linhas, resto = resto.split(b"\n")
            agora = time.monotonic()
            hora_ms = int(time.time() * 1000) + deslocamento_ms
            for bruta in linhas:
                mostrar_mensagens(conferencia.linha(bruta, hora_ms))
                if conferencia.ultima is not None:
                    if salvar is not None:
                        salvar.write(mostrar(bruta) + "\n")
                    mandar(pi.recebeu(conferencia.ultima, agora, hora_ms), hora_ms)
            mandar(pi.tique(agora, hora_ms), hora_ms)
            mandar(pi.agendadas(agora), hora_ms, da_ferramenta=False)
            while not teclado.empty():
                mandar([enmoldurar(teclado.get())], hora_ms, da_ferramenta=False)
    except KeyboardInterrupt:
        pass
    finally:
        porta.close()
        if salvar is not None:
            salvar.close()
    # O pedaço sem \n no fim foi cortado pela própria captura e não conta.
    for mensagem in conferencia.terminar():
        if args.segundos is None or rara_ou_problema(mensagem):
            print(f"{time.monotonic() - inicio:9.3f}  {mensagem}")

    print(f"\nResumo de {time.monotonic() - inicio:.1f} s em {nome_porta}:")
    print(conferencia.resumo())
    return 1 if conferencia.falhou() else 0


if __name__ == "__main__":
    sys.exit(main())
