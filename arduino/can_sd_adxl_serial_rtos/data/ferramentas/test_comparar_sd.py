"""Testes da comparação entre a captura e o cartão, sem cartão nem placa.

    python -m unittest discover -s ferramentas
"""
import unittest

from bancada import checksum
from comparar_sd import Arquivo, comparar, ler_arquivo, nome_do_arquivo, sem_moldura

CABECALHO = "# V-GUARD 0.7.0-1a2b3c4 | B,seq,t,firmware,motivo_reset | H,unix_ms"


def moldura(texto: str) -> str:
    return f"${texto}*{checksum(texto.encode()):02X}"


# 08/11/2026 00:59:50 UTC, 10 s antes da virada.
H = 1_794_099_590_000

CAPTURA = [
    moldura("B,0,0,0.7.0-1a2b3c4,LIGOU"),
    moldura("S,1,0,0,ESPERA,PARADO,OK,OK,97,12.5,0"),
    moldura(f"H,{H}"),
    moldura(f"S,2,{H + 300},1,ATIVO,ESCUTANDO,OK,OK,97,12.5,0"),
    moldura(f"A,3,{H + 9_980},0.12,-0.05,9.79"),
    moldura(f"A,4,{H + 10_000},0.12,-0.05,9.79"),
]

CARTAO = [
    Arquivo("20261108-00.csv", 1, ["B,0,0,0.7.0-1a2b3c4,LIGOU", f"H,{H}",
                                   f"S,2,{H + 300},1,ATIVO,ESCUTANDO,OK,OK,97,12.5,0",
                                   f"A,3,{H + 9_980},0.12,-0.05,9.79"]),
    Arquivo("20261108-01.csv", 1, [f"A,4,{H + 10_000},0.12,-0.05,9.79"]),
]


class TestComparacao(unittest.TestCase):
    def test_cartao_igual_passa(self):
        resultado = comparar(CAPTURA, CARTAO)
        self.assertEqual((6, 5, 5), (resultado.capturadas, resultado.esperadas,
                                     resultado.no_cartao_na_janela))
        self.assertFalse(resultado.falhou())

    def test_linha_que_falta(self):
        cartao = [CARTAO[0], Arquivo("20261108-01.csv", 1, [])]
        resultado = comparar(CAPTURA, cartao)
        self.assertEqual([f"A,4,{H + 10_000},0.12,-0.05,9.79"], resultado.faltam)
        self.assertTrue(resultado.falhou())

    def test_linha_que_sobra_na_janela(self):
        # Uma linha descartada na serial está no cartão e não na captura.
        cartao = [Arquivo(CARTAO[0].nome, 1, CARTAO[0].linhas + [f"A,9,{H + 9_990},0,0,9.81"]),
                  CARTAO[1]]
        resultado = comparar(CAPTURA, cartao)
        self.assertEqual([f"A,9,{H + 9_990},0,0,9.81"], resultado.sobram)
        self.assertTrue(resultado.falhou())

    def test_fora_da_janela_nao_conta(self):
        # Um boot anterior e linhas depois do fim da captura.
        antes = ["B,0,0,0.6.0-0000000,LIGOU", f"H,{H - 1_000}", f"A,1,{H - 900},0,0,9.81"]
        cartao = [Arquivo(CARTAO[0].nome, 2, antes + CARTAO[0].linhas),
                  Arquivo(CARTAO[1].nome, 1, CARTAO[1].linhas + [f"A,5,{H + 10_020},0,0,9.81"])]
        self.assertFalse(comparar(CAPTURA, cartao).falhou())

    def test_linha_na_hora_errada(self):
        cartao = [Arquivo(CARTAO[0].nome, 1, CARTAO[0].linhas + CARTAO[1].linhas)]
        resultado = comparar(CAPTURA, cartao)
        self.assertEqual([("20261108-00.csv", f"A,4,{H + 10_000},0.12,-0.05,9.79")],
                         resultado.hora_errada)
        self.assertTrue(resultado.falhou())

    def test_h_invalido_nao_e_esperado(self):
        captura = CAPTURA + [moldura("H,0")]
        self.assertFalse(comparar(captura, CARTAO).falhou())

    def test_linha_repetida_conta_duas_vezes(self):
        captura = CAPTURA + [moldura(f"H,{H + 5_000}"), moldura(f"H,{H + 5_000}")]
        cartao = [Arquivo(CARTAO[0].nome, 1, CARTAO[0].linhas + [f"H,{H + 5_000}"]), CARTAO[1]]
        self.assertEqual([f"H,{H + 5_000}"] * 2, comparar(captura, cartao).faltam)


class TestLeitura(unittest.TestCase):
    def test_ler_arquivo_conta_os_cabecalhos(self):
        arquivo = ler_arquivo("20261108-00.csv", f"{CABECALHO}\r\nH,{H}\n{CABECALHO}\nH,{H + 1}\n")
        self.assertEqual(2, arquivo.cabecalhos)
        self.assertEqual([f"H,{H}", f"H,{H + 1}"], arquivo.linhas)

    def test_sem_moldura(self):
        self.assertEqual("A,57,1,0.12,-0.05,9.79", sem_moldura("$A,57,1,0.12,-0.05,9.79*6D\r\n"))
        self.assertIsNone(sem_moldura("A,57,1"))

    def test_nome_do_arquivo_igual_ao_do_firmware(self):
        self.assertEqual("20261108-00.csv", nome_do_arquivo(1_794_099_493_000))
        self.assertEqual("20280229-23.csv", nome_do_arquivo(1_835_481_540_000))


if __name__ == "__main__":
    unittest.main()
