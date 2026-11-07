// Testes do protocolo da serial (lib/nucleo/protocolo), com os exemplos da proposta §5.6.
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "protocolo.h"

using namespace vguard::protocolo;

namespace {

// Guarda as linhas recebidas. Com cheio = true, recusa como a saída serial sem espaço.
class DestinoDeTeste final : public Destino {
 public:
  bool escrever(const char *linha, size_t tamanho) override {
    if (cheio || quantidade >= maximo_linhas || tamanho > tamanho_maximo + 1) {
      return false;
    }
    std::memcpy(linhas[quantidade], linha, tamanho);
    linhas[quantidade][tamanho] = '\0';
    ++quantidade;
    return true;
  }

  static constexpr size_t maximo_linhas = 8;
  char linhas[maximo_linhas][tamanho_maximo + 2] = {};
  size_t quantidade = 0;
  bool cheio = false;
};

// O $S do exemplo da proposta: ESPERA, PARADO, OK, OK, 97% livre, 12.5 pedidos/s.
Status status_do_exemplo() {
  return Status{2, Estado::espera, EstadoObd::parado, EstadoImu::ok, EstadoSd::ok, 97, 125};
}

// O $S deste ticket: só ESPERA, com as peças que ainda não existem em valores fixos.
Status status_sem_pecas(uint32_t uptime_s) {
  return Status{uptime_s,         Estado::espera, EstadoObd::parado, EstadoImu::falha,
                EstadoSd::ausente, 0,             125};
}

}  // namespace

void setUp() {}

void tearDown() {}

static void teste_checksum_e_o_xor_dos_caracteres() {
  const char texto[] = "B,0,0,0.3.0-a58af37,QUEDA_TENSAO";
  TEST_ASSERT_EQUAL_HEX8(0x2A, checksum(texto, std::strlen(texto)));
  TEST_ASSERT_EQUAL_HEX8(0x00, checksum(texto, 0));
}

static void teste_linha_do_boot_do_exemplo() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_boot(0, "0.3.0-a58af37", MotivoReset::queda_tensao);
  TEST_ASSERT_EQUAL_UINT(1, destino.quantidade);
  TEST_ASSERT_EQUAL_STRING("$B,0,0,0.3.0-a58af37,QUEDA_TENSAO*2A\n", destino.linhas[0]);
}

static void teste_linha_de_status_do_exemplo() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_boot(0, "0.3.0-a58af37", MotivoReset::queda_tensao);
  emissor.emitir_status(0, status_do_exemplo());
  TEST_ASSERT_EQUAL_STRING("$S,1,0,2,ESPERA,PARADO,OK,OK,97,12.5,0*5F\n", destino.linhas[1]);
}

static void teste_status_sem_pecas_nao_finge_funcionar() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_status(0, status_sem_pecas(0));
  // O checksum, conferido à parte: XOR de "S,0,0,0,ESPERA,PARADO,FALHA,AUSENTE,0,12.5,0".
  const char texto[] = "S,0,0,0,ESPERA,PARADO,FALHA,AUSENTE,0,12.5,0";
  char esperado[tamanho_maximo + 2];
  std::snprintf(esperado, sizeof(esperado), "$%s*%02X\n", texto,
                checksum(texto, std::strlen(texto)));
  TEST_ASSERT_EQUAL_STRING(esperado, destino.linhas[0]);
}

static void teste_seq_comeca_em_0_e_soma_1_por_linha() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_boot(0, "0.2.0-abcdef0", MotivoReset::ligou);
  emissor.emitir_status(0, status_sem_pecas(0));
  emissor.emitir_status(0, status_sem_pecas(1));
  TEST_ASSERT_EQUAL_UINT(3, destino.quantidade);
  TEST_ASSERT_EQUAL_STRING_LEN("$B,0,", destino.linhas[0], 5);
  TEST_ASSERT_EQUAL_STRING_LEN("$S,1,", destino.linhas[1], 5);
  TEST_ASSERT_EQUAL_STRING_LEN("$S,2,", destino.linhas[2], 5);
}

static void teste_linha_que_nao_cabe_e_descartada_e_gasta_a_seq() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_status(0, status_sem_pecas(0));  // seq 0, sai
  destino.cheio = true;
  emissor.emitir_status(0, status_sem_pecas(1));  // seq 1, não cabe
  TEST_ASSERT_EQUAL_UINT32(1, emissor.descartadas());
  destino.cheio = false;
  emissor.emitir_status(0, status_sem_pecas(2));  // seq 2, sai e conta a descartada
  TEST_ASSERT_EQUAL_UINT(2, destino.quantidade);
  TEST_ASSERT_EQUAL_STRING_LEN("$S,2,0,2,", destino.linhas[1], 9);
  // O último campo, antes do *hh, é o das descartadas.
  const char *asterisco = std::strchr(destino.linhas[1], '*');
  TEST_ASSERT_NOT_NULL(asterisco);
  TEST_ASSERT_EQUAL_STRING_LEN(",1*", asterisco - 2, 3);
}

static void teste_todos_os_motivos_de_reset() {
  TEST_ASSERT_EQUAL_STRING("LIGOU", nome(MotivoReset::ligou));
  TEST_ASSERT_EQUAL_STRING("SOFTWARE", nome(MotivoReset::software));
  TEST_ASSERT_EQUAL_STRING("PANE", nome(MotivoReset::pane));
  TEST_ASSERT_EQUAL_STRING("WATCHDOG", nome(MotivoReset::watchdog));
  TEST_ASSERT_EQUAL_STRING("QUEDA_TENSAO", nome(MotivoReset::queda_tensao));
  TEST_ASSERT_EQUAL_STRING("OUTRO", nome(MotivoReset::outro));
}

static void teste_nomes_dos_campos_do_status() {
  TEST_ASSERT_EQUAL_STRING("ATIVO", nome(Estado::ativo));
  TEST_ASSERT_EQUAL_STRING("ESCUTANDO", nome(EstadoObd::escutando));
  TEST_ASSERT_EQUAL_STRING("DESCOBRINDO", nome(EstadoObd::descobrindo));
  TEST_ASSERT_EQUAL_STRING("LENDO", nome(EstadoObd::lendo));
  TEST_ASSERT_EQUAL_STRING("SEM_RESPOSTA", nome(EstadoObd::sem_resposta));
  TEST_ASSERT_EQUAL_STRING("FALHA", nome(EstadoObd::falha));
  TEST_ASSERT_EQUAL_STRING("CHEIO", nome(EstadoSd::cheio));
  TEST_ASSERT_EQUAL_STRING("FALHA", nome(EstadoSd::falha));
}

static void teste_obd_hz_com_ate_1_casa() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  Status status = status_sem_pecas(0);
  status.obd_hz_decimos = 150;
  emissor.emitir_status(0, status);
  status.obd_hz_decimos = 55;
  emissor.emitir_status(0, status);
  TEST_ASSERT_NOT_NULL(std::strstr(destino.linhas[0], ",0,15,0*"));
  TEST_ASSERT_NOT_NULL(std::strstr(destino.linhas[1], ",0,5.5,0*"));
}

static void teste_t_com_13_digitos() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_boot(1793980812437ULL, "0.2.0-abcdef0", MotivoReset::ligou);
  TEST_ASSERT_EQUAL_STRING_LEN("$B,0,1793980812437,", destino.linhas[0], 19);
}

static void teste_campo_decimal() {
  Linha linha('A');
  linha.campo_decimal(12, 2).campo_decimal(-5, 2).campo_decimal(979, 2).campo_decimal(87625, 2);
  linha.campo_decimal(-1380, 1).campo_decimal(13800, 3).campo_decimal(7, 0);
  TEST_ASSERT_TRUE(linha.fechar());
  const char esperado[] = "$A,0.12,-0.05,9.79,876.25,-138.0,13.800,7*";
  TEST_ASSERT_EQUAL_STRING_LEN(esperado, linha.dados(), std::strlen(esperado));
}

static void teste_linha_de_120_caracteres_passa_e_a_de_121_nao() {
  // "$X," mais o texto mais "*hh": 3 + 114 + 3 = 120.
  char texto[116];
  std::memset(texto, 'a', 114);
  texto[114] = '\0';
  Linha cabe('X');
  cabe.campo_texto(texto);
  TEST_ASSERT_TRUE(cabe.fechar());
  TEST_ASSERT_EQUAL_UINT(tamanho_maximo + 1, cabe.tamanho());  // com o \n

  texto[114] = 'a';
  texto[115] = '\0';
  Linha nao_cabe('X');
  nao_cabe.campo_texto(texto);
  TEST_ASSERT_FALSE(nao_cabe.fechar());
}

static void teste_texto_com_caractere_da_moldura_invalida_a_linha() {
  const char *proibidos[] = {"a$b", "a*b", "a\nb", "a\rb"};
  for (const char *texto : proibidos) {
    Linha linha('L');
    linha.campo_texto(texto);
    TEST_ASSERT_FALSE(linha.fechar());
  }
  Linha sem_texto('L');
  sem_texto.campo_texto(nullptr);
  TEST_ASSERT_FALSE(sem_texto.fechar());
}

static void teste_linha_invalida_conta_como_descartada() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_boot(0, "versao*com-asterisco", MotivoReset::ligou);
  TEST_ASSERT_EQUAL_UINT(0, destino.quantidade);
  TEST_ASSERT_EQUAL_UINT32(1, emissor.descartadas());
}

static void teste_aviso_do_exemplo() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  // As 70 primeiras linhas não cabem, só para a seq chegar à do exemplo da proposta.
  destino.cheio = true;
  for (int i = 0; i < 70; ++i) {
    emissor.emitir_status(0, status_sem_pecas(0));
  }
  destino.cheio = false;
  emissor.emitir_log(1793980813200ULL, Nivel::aviso, "PID 0F sem resposta");
  TEST_ASSERT_EQUAL_STRING("$L,70,1793980813200,AVISO,PID 0F sem resposta*51\n",
                           destino.linhas[0]);
}

static void teste_erro_e_texto_cortado_em_60_caracteres() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  char texto[71];
  std::memset(texto, 'x', 70);
  texto[70] = '\0';
  emissor.emitir_log(0, Nivel::erro, texto);
  TEST_ASSERT_EQUAL_UINT(1, destino.quantidade);
  // Só os 60 primeiros caracteres do texto entram.
  char corpo[tamanho_maximo + 1];
  std::snprintf(corpo, sizeof(corpo), "L,0,0,ERRO,%.60s", texto);
  // Folga para o compilador ver que o snprintf não corta (o -Werror=format-truncation).
  char esperado[sizeof(corpo) + 5];
  std::snprintf(esperado, sizeof(esperado), "$%s*%02X\n", corpo,
                checksum(corpo, std::strlen(corpo)));
  TEST_ASSERT_EQUAL_STRING(esperado, destino.linhas[0]);
}

static void teste_aviso_com_caractere_da_moldura_e_descartado() {
  DestinoDeTeste destino;
  Emissor emissor(destino);
  emissor.emitir_log(0, Nivel::aviso, "a*b");
  TEST_ASSERT_EQUAL_UINT(0, destino.quantidade);
  TEST_ASSERT_EQUAL_UINT32(1, emissor.descartadas());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(teste_aviso_do_exemplo);
  RUN_TEST(teste_erro_e_texto_cortado_em_60_caracteres);
  RUN_TEST(teste_aviso_com_caractere_da_moldura_e_descartado);
  RUN_TEST(teste_checksum_e_o_xor_dos_caracteres);
  RUN_TEST(teste_linha_do_boot_do_exemplo);
  RUN_TEST(teste_linha_de_status_do_exemplo);
  RUN_TEST(teste_status_sem_pecas_nao_finge_funcionar);
  RUN_TEST(teste_seq_comeca_em_0_e_soma_1_por_linha);
  RUN_TEST(teste_linha_que_nao_cabe_e_descartada_e_gasta_a_seq);
  RUN_TEST(teste_todos_os_motivos_de_reset);
  RUN_TEST(teste_nomes_dos_campos_do_status);
  RUN_TEST(teste_obd_hz_com_ate_1_casa);
  RUN_TEST(teste_t_com_13_digitos);
  RUN_TEST(teste_campo_decimal);
  RUN_TEST(teste_linha_de_120_caracteres_passa_e_a_de_121_nao);
  RUN_TEST(teste_texto_com_caractere_da_moldura_invalida_a_linha);
  RUN_TEST(teste_linha_invalida_conta_como_descartada);
  return UNITY_END();
}
