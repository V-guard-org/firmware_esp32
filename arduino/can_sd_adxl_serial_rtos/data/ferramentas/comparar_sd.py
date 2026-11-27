"""Compara a captura da ferramenta de bancada (--salvar) com os arquivos do microSD (proposta
§4.5).

    python ferramentas/comparar_sd.py captura.txt /media/cartao/*.csv

O cartão deve ter as linhas da captura, sem o $ e sem o *hh, mais os $H mandados:
- as linhas com t, o $B de cada boot e os $H válidos (a partir de 01/01/2026). As linhas com t 0
  (antes da hora) não vão para o cartão, a não ser o $B;
- fora da janela da captura (t antes da primeira ou depois da última linha capturada), o que o
  cartão tem a mais não conta: é de antes ou de depois da captura. O $B entra na janela quando a
  linha seguinte do cartão é um $H dentro dela.

Também confere cada arquivo: quantos cabeçalhos (#) ele tem e se o t de cada linha está na hora
UTC do nome. Sai com 1 se falta ou sobra linha, ou se alguma está no arquivo de outra hora.

As linhas que o ESP32 descartou na serial (descartadas do $S) estão no cartão e não na captura:
aparecem como sobrando. Na bancada, o descartadas deve ficar em 0.
"""
import collections
import dataclasses
import datetime
import sys
from pathlib import Path

# O $H só vale a partir de 01/01/2026 00:00 UTC (proposta §5.4).
HORA_MINIMA_MS = 1_767_225_600_000
MOSTRAR_NO_MAXIMO = 10


def sem_moldura(linha: str) -> str | None:
    """"$A,57,...*hh" vira "A,57,..."; sem a moldura, None."""
    linha = linha.strip()
    if len(linha) < 5 or not linha.startswith("$") or linha[-3] != "*":
        return None
    return linha[1:-3]


def t_da_linha(texto: str) -> int:
    """O t da linha sem moldura: o 3º campo, ou o 2º no $H, que não tem seq. 0 se não há."""
    campos = texto.split(",")
    posicao = 1 if campos[0] == "H" else 2
    if len(campos) <= posicao or not campos[posicao].isdigit():
        return 0
    return int(campos[posicao])


def nome_do_arquivo(t: int) -> str:
    """O arquivo da hora UTC do t, como o firmware: AAAAMMDD-HH.csv."""
    return datetime.datetime.fromtimestamp(t / 1000, datetime.timezone.utc).strftime(
        "%Y%m%d-%H.csv")


def esperada_no_cartao(texto: str) -> bool:
    tipo = texto.split(",")[0]
    if tipo == "H":
        return t_da_linha(texto) >= HORA_MINIMA_MS
    return tipo == "B" or t_da_linha(texto) != 0


@dataclasses.dataclass
class Arquivo:
    nome: str
    cabecalhos: int
    linhas: list[str]


@dataclasses.dataclass
class Resultado:
    esperadas: int
    capturadas: int
    no_cartao_na_janela: int
    faltam: list[str]
    sobram: list[str]
    hora_errada: list[tuple[str, str]]

    def falhou(self) -> bool:
        return bool(self.faltam or self.sobram or self.hora_errada)


def ler_arquivo(nome: str, conteudo: str) -> Arquivo:
    linhas = [linha.rstrip("\r") for linha in conteudo.split("\n") if linha.strip()]
    return Arquivo(nome=nome, cabecalhos=sum(linha.startswith("#") for linha in linhas),
                   linhas=[linha for linha in linhas if not linha.startswith("#")])


def comparar(captura: list[str], arquivos: list[Arquivo]) -> Resultado:
    """captura: as linhas do --salvar, com a moldura; arquivos: os do cartão, em ordem de nome."""
    textos = [texto for texto in map(sem_moldura, captura) if texto is not None]
    esperadas = [texto for texto in textos if esperada_no_cartao(texto)]
    tempos = [t_da_linha(texto) for texto in esperadas if t_da_linha(texto) != 0]
    if not tempos:
        return Resultado(len(esperadas), len(textos), 0, esperadas, [], [])
    inicio, fim = min(tempos), max(tempos)

    na_janela = []
    hora_errada = []
    for arquivo in arquivos:
        for i, texto in enumerate(arquivo.linhas):
            t = t_da_linha(texto)
            if t != 0 and nome_do_arquivo(t) != arquivo.nome:
                hora_errada.append((arquivo.nome, texto))
            if t != 0:
                dentro = inicio <= t <= fim
            else:
                seguinte = arquivo.linhas[i + 1] if i + 1 < len(arquivo.linhas) else ""
                dentro = (seguinte.startswith("H,")
                          and inicio <= t_da_linha(seguinte) <= fim)
            if dentro:
                na_janela.append(texto)

    contagem_esperada = collections.Counter(esperadas)
    contagem_cartao = collections.Counter(na_janela)
    faltam = [texto for texto in esperadas if contagem_esperada[texto] > contagem_cartao[texto]]
    sobram = [texto for texto in na_janela if contagem_cartao[texto] > contagem_esperada[texto]]
    return Resultado(len(esperadas), len(textos), len(na_janela), faltam, sobram, hora_errada)


def mostrar_hora(t: int) -> str:
    return datetime.datetime.fromtimestamp(t / 1000, datetime.timezone.utc).strftime(
        "%H:%M:%S.%f")[:-3]


def resumo(arquivos: list[Arquivo], resultado: Resultado) -> str:
    saida = [f"Cartão: {len(arquivos)} arquivo(s)"]
    for arquivo in arquivos:
        tempos = [t for t in map(t_da_linha, arquivo.linhas) if t != 0]
        janela = (f", t de {mostrar_hora(min(tempos))} a {mostrar_hora(max(tempos))} UTC"
                  if tempos else "")
        saida.append(f"  {arquivo.nome}: {arquivo.cabecalhos} cabeçalho(s), "
                     f"{len(arquivo.linhas)} linhas{janela}")
    saida.append(f"Captura: {resultado.capturadas} linhas, {resultado.esperadas} esperadas no "
                 f"cartão; no cartão, {resultado.no_cartao_na_janela} na janela da captura")
    for titulo, linhas in (("Faltam no cartão", resultado.faltam),
                           ("Sobram no cartão", resultado.sobram)):
        saida.append(f"{titulo}: {len(linhas)}")
        saida += [f"  {texto}" for texto in linhas[:MOSTRAR_NO_MAXIMO]]
    saida.append(f"No arquivo de outra hora: {len(resultado.hora_errada)}")
    saida += [f"  {nome}: {texto}" for nome, texto in resultado.hora_errada[:MOSTRAR_NO_MAXIMO]]
    return "\n".join(saida)


def main() -> int:
    if len(sys.argv) < 3:
        sys.exit(__doc__.split("\n\n")[1])
    captura = Path(sys.argv[1]).read_text(encoding="ascii", errors="replace").splitlines()
    caminhos = sorted((Path(p) for p in sys.argv[2:]), key=lambda caminho: caminho.name)
    arquivos = [ler_arquivo(c.name, c.read_text(encoding="ascii", errors="replace"))
                for c in caminhos]
    resultado = comparar(captura, arquivos)
    print(resumo(arquivos, resultado))
    return 1 if resultado.falhou() else 0


if __name__ == "__main__":
    sys.exit(main())
