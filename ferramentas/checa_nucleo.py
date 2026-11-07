"""Confere as regras mecânicas do núcleo (CODING_STANDARDS.md): só memória de tamanho fixo
e nada do hardware. Falha com a lista do que achou.

    python ferramentas/checa_nucleo.py

Comentários e textos entre aspas não contam, para um "new" num comentário não acusar.
"""
import pathlib
import re
import sys

NUCLEO = pathlib.Path(__file__).resolve().parent.parent / "lib" / "nucleo"
EXTENSOES = {".h", ".hpp", ".c", ".cpp"}

PROIBIDOS = [
    # O "= delete;" apaga uma função, não devolve memória.
    (re.compile(r"\bnew\b|\bdelete\b(?!\s*;)"), "memória dinâmica (new/delete)"),
    (re.compile(r"\b(malloc|calloc|realloc|free)\s*\("), "memória dinâmica (malloc/free)"),
    (re.compile(r"\bString\b"), "String do Arduino, que aloca memória"),
    (re.compile(r"\bstd::(vector|string|basic_string|map|multimap|set|multiset|unordered_\w+"
                r"|list|forward_list|deque|function|shared_ptr|unique_ptr|make_shared"
                r"|make_unique)\b"), "contêiner da biblioteca padrão, que aloca memória"),
    (re.compile(r"#\s*include\s*<(vector|string|map|set|unordered_\w+|list|forward_list"
                r"|deque|functional|memory)>"), "cabeçalho de contêiner que aloca memória"),
    (re.compile(r"#\s*include\s*[<\"](Arduino|SPI|Wire|SD|FS|mcp_can|HardwareSerial)\.h[>\"]"),
     "hardware no núcleo"),
    (re.compile(r"#\s*include\s*[<\"](freertos/|esp_|driver/)"), "hardware no núcleo"),
]


def sem_comentarios_e_textos(codigo):
    """Troca comentários e textos entre aspas por espaços, mantendo as quebras de linha."""
    padrao = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'',
                        re.DOTALL)

    def apaga(m):
        texto = m.group(0)
        # Mantém o caminho dos #include "..."; só o conteúdo de textos comuns some.
        if texto.startswith('"') and re.search(r'#\s*include\s*$', codigo[:m.start()].rsplit("\n", 1)[-1]):
            return texto
        return re.sub(r"[^\n]", " ", texto)

    return padrao.sub(apaga, codigo)


def main():
    achados = []
    for arquivo in sorted(NUCLEO.rglob("*")):
        if arquivo.suffix not in EXTENSOES:
            continue
        linhas = sem_comentarios_e_textos(arquivo.read_text(encoding="utf-8")).split("\n")
        for numero, linha in enumerate(linhas, start=1):
            for padrao, motivo in PROIBIDOS:
                if padrao.search(linha):
                    caminho = arquivo.relative_to(NUCLEO.parent.parent)
                    achados.append(f"{caminho}:{numero}: {motivo}: {linha.strip()}")
    for achado in achados:
        print(achado)
    if achados:
        print(f"\n{len(achados)} problema(s) no núcleo. As regras estão no CODING_STANDARDS.md.")
        return 1
    print("Núcleo ok: só memória de tamanho fixo e nada do hardware.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
