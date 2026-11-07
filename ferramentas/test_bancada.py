"""Testes da conferência da ferramenta de bancada, sem porta nem placa.

    python -m unittest discover -s ferramentas
"""
import unittest

from bancada import Conferencia, Valida, checksum, ler_linha


def moldura(corpo: str) -> bytes:
    return f"${corpo}*{checksum(corpo.encode()):02X}".encode()


def status(seq: int, uptime: int) -> bytes:
    return moldura(f"S,{seq},0,{uptime},ESPERA,PARADO,FALHA,AUSENTE,0,12.5,0")


def boot() -> bytes:
    return moldura("B,0,0,0.2.0-abcdef0,LIGOU")


ROM = [b"ets Jun  8 2016 00:22:57", b"", b"rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)"]


class TestMoldura(unittest.TestCase):
    def test_exemplos_da_proposta(self):
        self.assertIsInstance(ler_linha(b"$B,0,0,0.3.0-a58af37,QUEDA_TENSAO*2A"), Valida)
        linha = ler_linha(b"$S,1,0,2,ESPERA,PARADO,OK,OK,97,12.5,0*5F\r")
        self.assertIsInstance(linha, Valida)
        self.assertEqual(("S", 1), (linha.tipo, linha.seq))

    def test_motivos_de_recusa(self):
        self.assertEqual("sem $", ler_linha(b"S,1,0*5F"))
        self.assertEqual("sem *hh", ler_linha(b"$S,1,0"))
        self.assertEqual("sem *hh", ler_linha(b"$S,1,0*5"))
        self.assertEqual("sem *hh", ler_linha(b"$S,1,0*5f"))
        self.assertIn("checksum errado", ler_linha(b"$S,1,0,2,ESPERA,PARADO,OK,OK,97,12.5,0*5E"))
        self.assertEqual("sem a seq", ler_linha(moldura("S,x,0")))

    def test_120_caracteres_passa_e_121_nao(self):
        cabe = moldura("X,0," + "a" * 112)
        self.assertEqual(120, len(cabe))
        self.assertIsInstance(ler_linha(cabe), Valida)
        self.assertIn("mais de 120", ler_linha(moldura("X,0," + "a" * 113)))


class TestConferencia(unittest.TestCase):
    def conferir(self, linhas):
        conferencia = Conferencia()
        for linha in linhas:
            conferencia.linha(linha)
        conferencia.terminar()
        return conferencia

    def test_boot_e_status_sem_problema(self):
        c = self.conferir(ROM + [boot()] + [status(n, n - 1) for n in range(1, 301)])
        self.assertEqual((301, 0, 0, 0, 0), (c.validas, c.corrompidas, c.perdidas,
                                             c.reinicios, c.saltos_uptime))
        self.assertEqual({"B": 1, "S": 300}, c.por_tipo)
        self.assertEqual(len(ROM), c.ignoradas)
        self.assertFalse(c.falhou())

    def test_lacuna_conta_as_perdidas(self):
        c = self.conferir([boot(), status(1, 0), status(4, 3)])
        self.assertEqual(2, c.perdidas)
        self.assertEqual(1, c.saltos_uptime)
        self.assertTrue(c.falhou())

    def test_reinicio_nao_e_lacuna_e_o_texto_do_boot_e_ignorado(self):
        cortada = b"$S,3,0,2,ESPE"  # o reinício cortou a linha no meio
        c = self.conferir([boot(), status(1, 0), status(2, 1), cortada] + ROM
                          + [boot(), status(1, 0)])
        self.assertEqual((1, 0, 0), (c.reinicios, c.perdidas, c.corrompidas))
        self.assertEqual(1 + len(ROM), c.ignoradas)
        self.assertEqual(0, c.saltos_uptime)
        self.assertFalse(c.falhou())

    def test_recusada_entre_validas_e_corrompida(self):
        c = self.conferir([boot(), status(1, 0), b"lixo sem dolar", status(2, 1)])
        self.assertEqual(1, c.corrompidas)
        self.assertTrue(c.falhou())

    def test_checksum_errado_e_corrompida(self):
        errada = bytearray(status(2, 1))
        errada[-1] = ord("0") if errada[-1] != ord("0") else ord("1")
        c = self.conferir([boot(), status(1, 0), bytes(errada), status(3, 2)])
        self.assertEqual(1, c.corrompidas)
        # A linha corrompida também deixa a lacuna da seq 2.
        self.assertEqual(1, c.perdidas)

    def test_comeco_no_meio_de_uma_linha_e_ignorado(self):
        c = self.conferir([b"FALHA,AUSENTE,0,12.5,0*5F", status(57, 56), status(58, 57)])
        self.assertEqual((0, 1, 2), (c.corrompidas, c.ignoradas, c.validas))
        self.assertFalse(c.falhou())

    def test_recusada_no_fim_da_captura_e_corrompida(self):
        c = self.conferir([boot(), status(1, 0), b"$S,2,0,1,ESPERA*00"])
        self.assertEqual(1, c.corrompidas)

    def test_seq_para_tras_e_fora_de_ordem(self):
        c = self.conferir([boot(), status(1, 0), status(2, 1), status(1, 2)])
        self.assertEqual(1, c.fora_de_ordem)
        self.assertTrue(c.falhou())

    def test_seq_passa_de_32_bits_para_0_sem_reinicio(self):
        c = self.conferir([moldura("S,4294967295,0,5,ESPERA"), moldura("S,0,0,6,ESPERA")])
        self.assertEqual((0, 0, 0), (c.reinicios, c.perdidas, c.fora_de_ordem))

    def test_sem_nenhuma_valida_falha(self):
        c = self.conferir(ROM)
        self.assertEqual(0, c.validas)
        self.assertTrue(c.falhou())


if __name__ == "__main__":
    unittest.main()
