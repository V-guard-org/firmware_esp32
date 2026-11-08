"""Testes da conferência da ferramenta de bancada, sem porta nem placa.

    python -m unittest discover -s ferramentas
"""
import unittest

from bancada import Conferencia, PapelDaPi, Valida, checksum, enmoldurar, ler_linha


def moldura(corpo: str) -> bytes:
    return f"${corpo}*{checksum(corpo.encode()):02X}".encode()


def status(seq: int, uptime: int) -> bytes:
    return moldura(f"S,{seq},0,{uptime},ESPERA,PARADO,FALHA,AUSENTE,0,12.5,0")


def boot() -> bytes:
    return moldura("B,0,0,0.2.0-abcdef0,LIGOU")


def ativo(seq: int, uptime: int, t: int) -> bytes:
    return moldura(f"S,{seq},{t},{uptime},ATIVO,PARADO,FALHA,AUSENTE,0,12.5,0")


def ativo_com(seq: int, uptime: int, imu: str = "OK", descartadas: int = 0) -> bytes:
    return moldura(f"S,{seq},{HORA},{uptime},ATIVO,PARADO,{imu},AUSENTE,0,12.5,{descartadas}")


def acel(seq: int, ax: str = "0.12", ay: str = "-0.05", az: str = "9.79", t: int = 0) -> bytes:
    return moldura(f"A,{seq},{t or HORA},{ax},{ay},{az}")


def valida(bruta: bytes) -> Valida:
    linha = ler_linha(bruta)
    assert isinstance(linha, Valida), linha
    return linha


HORA = 1793980812437  # a hora do exemplo da proposta, em ms


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


class TestConferenciaDoAcelerometro(unittest.TestCase):
    """O $A entre dois $S, as descartadas, o imu e as médias (#4)."""

    def segundo(self, seq: int, quantidade: int) -> list[bytes]:
        """quantidade $A a partir da seq, e o $S do fim do segundo."""
        return [acel(seq + i) for i in range(quantidade)] + [ativo_com(seq + quantidade, 0)]

    def conferir(self, linhas):
        conferencia = Conferencia()
        mensagens = []
        for linha in linhas:
            mensagens += conferencia.linha(linha)
        mensagens += conferencia.terminar()
        return conferencia, mensagens

    def test_50_por_segundo_passa(self):
        linhas = ([ativo_com(0, 0)] + self.segundo(1, 50) + self.segundo(52, 49)
                  + self.segundo(102, 51))
        c, _ = self.conferir(linhas)
        self.assertEqual((0, 49, 51), (c.fora_da_taxa, c.menor_a_por_status, c.maior_a_por_status))
        self.assertFalse(c.falhou())

    def test_fora_de_49_a_51_falha(self):
        c, mensagens = self.conferir([ativo_com(0, 0)] + self.segundo(1, 47) + self.segundo(49, 52))
        self.assertEqual(2, c.fora_da_taxa)
        self.assertTrue(any("47 $A" in m for m in mensagens))
        self.assertTrue(c.falhou())

    def test_segundo_que_entra_em_atividade_nao_conta(self):
        # Do $S em ESPERA ao primeiro em ATIVO, o $A só começou no meio.
        c, _ = self.conferir([status(0, 0)] + self.segundo(1, 20) + self.segundo(22, 50))
        self.assertEqual((0, 50), (c.fora_da_taxa, c.menor_a_por_status))

    def test_reinicio_recomeca_a_contagem(self):
        linhas = ([ativo_com(0, 0)] + [acel(1), acel(2)] + ROM + [boot(), status(1, 0)]
                  + self.segundo(2, 30) + self.segundo(33, 50))
        c, _ = self.conferir(linhas)
        self.assertEqual(0, c.fora_da_taxa)

    def test_a_para_e_volta(self):
        linhas = ([ativo_com(0, 0)] + self.segundo(1, 50) + self.segundo(52, 10)
                  + [ativo_com(63, 0, imu="FALHA"), ativo_com(64, 0, imu="FALHA")]
                  + self.segundo(65, 30) + self.segundo(96, 50))
        c, mensagens = self.conferir(linhas)
        # O segundo com 10, os 2 sem nenhum e o com 30.
        self.assertEqual(4, c.fora_da_taxa)
        self.assertEqual(1, sum("o $A parou" in m for m in mensagens))
        self.assertEqual(1, sum("o $A voltou" in m for m in mensagens))
        self.assertIn("-- imu: OK → FALHA", mensagens)
        self.assertIn("-- imu: FALHA → OK", mensagens)
        self.assertTrue(c.falhou())

    def test_descartadas_falha_e_soma_o_que_subiu(self):
        linhas = [ativo_com(0, 0), ativo_com(1, 0, descartadas=2), ativo_com(2, 0, descartadas=5)]
        c, mensagens = self.conferir(linhas)
        self.assertEqual(5, c.descartadas)
        self.assertEqual(2, sum("descartou" in m for m in mensagens))
        self.assertTrue(c.falhou())

    def test_descartadas_recomecam_no_reinicio(self):
        linhas = ([ativo_com(0, 0, descartadas=3)] + [boot(), ativo_com(1, 0, descartadas=1)])
        c, _ = self.conferir(linhas)
        self.assertEqual(4, c.descartadas)

    def test_medias_no_resumo(self):
        linhas = [acel(0, "0.10", "-0.10", "9.70"), acel(1, "0.30", "0.10", "9.90")]
        c, _ = self.conferir(linhas)
        self.assertEqual(2, c.quantidade_a)
        self.assertIn("ax +0.20, ay +0.00, az +9.80, |a| 9.80 m/s² (2 linhas)", c.resumo())

    def test_sem_imu_nao_confere_a_taxa(self):
        conferencia = Conferencia(conferir_taxa_do_a=False)
        for linha in [ativo_com(0, 0, imu="FALHA"), ativo_com(1, 0, imu="FALHA"),
                      ativo_com(2, 0, imu="FALHA", descartadas=1)]:
            conferencia.linha(linha)
        conferencia.terminar()
        self.assertEqual(0, conferencia.fora_da_taxa)
        self.assertIn("não conferido (--sem-imu)", conferencia.resumo())
        # As descartadas continuam conferidas.
        self.assertEqual(1, conferencia.descartadas)
        self.assertTrue(conferencia.falhou())

    def test_imu_aparece_no_primeiro_status(self):
        _, mensagens = self.conferir([boot(), status(1, 0)])
        self.assertIn("-- imu: FALHA", mensagens)


class TestEnmoldurar(unittest.TestCase):
    def test_exemplos_da_proposta(self):
        self.assertEqual(b"$H,1793980812437*52\n", enmoldurar("H,1793980812437"))
        self.assertEqual(b"$C,obd_hz,15*63\n", enmoldurar("C,obd_hz,15\n"))

    def test_linha_com_dolar_vai_como_esta(self):
        self.assertEqual(b"$H,1*00\n", enmoldurar("$H,1*00"))


class TestConferenciaDaHora(unittest.TestCase):
    def test_t_perto_do_notebook_passa(self):
        c = Conferencia()
        c.linha(boot(), HORA)
        c.linha(ativo(1, 1, HORA - 20), HORA)
        for seq in range(2, 52):  # os 50 $A do segundo
            c.linha(acel(seq, t=HORA + 500), HORA + 500)
        c.linha(ativo(52, 2, HORA + 980), HORA + 1000)
        c.terminar()
        self.assertEqual(0, c.fora_da_hora)
        self.assertEqual(-20, c.maior_distancia_ms)
        self.assertFalse(c.falhou())

    def test_t_a_mais_de_1_s_falha(self):
        c = Conferencia()
        mensagens = c.linha(ativo(1, 1, HORA - 1500), HORA)
        c.linha(ativo(2, 2, HORA + 990), HORA + 1000)  # voltou para perto
        self.assertEqual(1, c.fora_da_hora)
        self.assertEqual(-1500, c.maior_distancia_ms)
        self.assertTrue(any("-1500 ms" in m for m in mensagens))
        self.assertTrue(c.falhou())

    def test_t_zero_nao_conta(self):
        c = Conferencia()
        c.linha(boot(), HORA)
        c.linha(status(1, 0), HORA)
        self.assertIsNone(c.maior_distancia_ms)
        self.assertFalse(c.falhou())

    def test_espera_ate_3_s_depois_do_h_passa(self):
        c = Conferencia()
        c.linha(boot(), HORA)
        c.hora_enviada(HORA)
        c.linha(status(1, 0), HORA + 100)  # já estava a caminho
        c.linha(ativo(2, 1, HORA + 1000), HORA + 1000)
        self.assertEqual(0, c.esperas)
        self.assertEqual(1, c.horas_enviadas)

    def test_espera_mais_de_3_s_depois_do_h_falha_uma_vez(self):
        c = Conferencia()
        c.linha(boot(), HORA)
        c.hora_enviada(HORA)
        for n in range(1, 7):
            c.linha(status(n, n - 1), HORA + 1000 * n)
        self.assertEqual(1, c.esperas)
        self.assertTrue(c.falhou())

    def test_reinicio_cancela_o_prazo_do_h(self):
        c = Conferencia()
        c.linha(boot(), HORA)
        c.hora_enviada(HORA)
        c.linha(status(1, 0), HORA + 500)
        c.linha(boot(), HORA + 2000)  # EN apertado antes do ESP32 sair de ESPERA
        c.linha(status(1, 0), HORA + 5000)
        self.assertEqual(0, c.esperas)

    def test_sem_hora_da_chegada_nao_confere(self):
        c = Conferencia()
        c.linha(ativo(1, 1, 5))
        c.terminar()
        self.assertIsNone(c.maior_distancia_ms)


class TestPapelDaPi(unittest.TestCase):
    def test_h_ao_ver_o_boot_com_a_hora_do_notebook(self):
        pi = PapelDaPi()
        self.assertEqual([enmoldurar(f"H,{HORA}")], pi.recebeu(valida(boot()), 10.0, HORA))
        # O $S em ESPERA que já estava a caminho não repete o $H.
        self.assertEqual([], pi.recebeu(valida(status(1, 0)), 10.1, HORA + 100))

    def test_h_de_novo_se_continua_em_espera(self):
        pi = PapelDaPi()
        pi.recebeu(valida(boot()), 10.0, HORA)
        self.assertEqual([enmoldurar(f"H,{HORA + 2000}")],
                         pi.recebeu(valida(status(2, 1)), 12.0, HORA + 2000))

    def test_h_a_cada_60_s(self):
        pi = PapelDaPi()
        pi.recebeu(valida(boot()), 10.0, HORA)
        pi.recebeu(valida(ativo(2, 1, HORA + 1000)), 11.0, HORA + 1000)
        self.assertEqual([], pi.tique(69.9, HORA + 59900))
        self.assertEqual([enmoldurar(f"H,{HORA + 60000}")], pi.tique(70.0, HORA + 60000))
        self.assertEqual([], pi.tique(70.1, HORA + 60100))

    def test_esp32_ja_ativo_ganha_h_na_primeira_linha(self):
        pi = PapelDaPi()
        self.assertEqual(1, len(pi.recebeu(valida(ativo(57, 56, HORA)), 0.0, HORA)))
        self.assertEqual([], pi.recebeu(valida(ativo(58, 57, HORA + 1000)), 1.0, HORA + 1000))

    def test_sem_hora_nunca_manda_h(self):
        pi = PapelDaPi(mandar_hora=False)
        self.assertEqual([], pi.recebeu(valida(boot()), 0.0, HORA))
        self.assertEqual([], pi.recebeu(valida(status(1, 0)), 5.0, HORA))
        self.assertEqual([], pi.tique(100.0, HORA))

    def test_enviar_com_2_s_de_intervalo(self):
        pi = PapelDaPi(mandar_hora=False, enviar=["C,obd_hz,15", "C,obd_hz,25"])
        self.assertEqual([], pi.agendadas(0.0))  # nenhuma linha válida ainda
        pi.recebeu(valida(status(1, 0)), 10.0, HORA)
        self.assertEqual([], pi.agendadas(11.9))
        self.assertEqual([b"$C,obd_hz,15*63\n"], pi.agendadas(12.0))
        self.assertEqual([enmoldurar("C,obd_hz,25")], pi.agendadas(14.5))
        self.assertEqual([], pi.agendadas(20.0))

    def test_enviar_nao_entra_no_h_de_60_s(self):
        pi = PapelDaPi(enviar=["H,0"])
        pi.recebeu(valida(boot()), 10.0, HORA)
        self.assertEqual([], pi.tique(12.0, HORA + 2000))
        self.assertEqual([b"$H,0*54\n"], pi.agendadas(12.0))


if __name__ == "__main__":
    unittest.main()
