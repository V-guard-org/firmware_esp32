// Testes da leitura das linhas da Pi (lib/nucleo/protocolo): a moldura, o $H, o $C e o
// relógio (proposta §5.1, §5.2 e §5.4).
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "protocolo.h"

using namespace vguard::protocolo;

namespace {

// Passa o texto ao leitor, caractere por caractere, e conta as linhas aceitas.
int aceitas(Leitor &leitor, const char *texto) {
  int quantidade = 0;
  for (const char *c = texto; *c != '\0'; ++c) {
    if (leitor.receber(*c)) {
      ++quantidade;
    }
  }
  return quantidade;
}

// Monta "$<corpo>*hh\n" com o checksum certo.
void moldura(const char *corpo, char *saida, size_t tamanho) {
  std::snprintf(saida, tamanho, "$%s*%02X\n", corpo, checksum(corpo, std::strlen(corpo)));
}

Comando ler(const char *corpo) {
  char linha[300];
  moldura(corpo, linha, sizeof(linha));
  Leitor leitor;
  for (const char *c = linha; *c != '\0'; ++c) {
    if (leitor.receber(*c)) {
      return interpretar(leitor.tipo(), leitor.campos());
    }
  }
  return Comando{};
}

}  // namespace

void setUp() {}

void tearDown() {}

// --- Moldura ---

static void teste_hora_do_exemplo_da_proposta() {
  Leitor leitor;
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, "$H,1793980812437*52\n"));
  TEST_ASSERT_EQUAL_CHAR('H', leitor.tipo());
  TEST_ASSERT_EQUAL_STRING("1793980812437", leitor.campos());
  const Comando comando = interpretar(leitor.tipo(), leitor.campos());
  TEST_ASSERT_EQUAL(Comando::Tipo::hora, comando.tipo);
  TEST_ASSERT_EQUAL_UINT64(1793980812437ULL, comando.unix_ms);
}

static void teste_aceita_cr_antes_do_lf() {
  Leitor leitor;
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, "$H,1793980812437*52\r\n"));
  TEST_ASSERT_EQUAL_STRING("1793980812437", leitor.campos());
}

static void teste_recusa_moldura_errada() {
  const char *erradas[] = {
      "H,1793980812437*52\n",     // sem $
      "$H,1793980812437\n",       // sem *hh
      "$H,1793980812437*5\n",     // checksum de 1 dígito
      "$H,1793980812437*53\n",    // checksum errado
      "$K*4b\n",                  // hexa minúsculo (o certo é 4B)
      "x$H,1793980812437*52\n",   // algo antes do $
      "$H,1793980812437*52 \n",   // algo depois do checksum
      "$H,17939808\r12437*52\n",  // \r no meio
      "\n",                       // linha vazia
      "$*00\n",                   // sem tipo
  };
  for (const char *linha : erradas) {
    Leitor leitor;
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, aceitas(leitor, linha), linha);
  }
}

static void teste_linha_de_120_caracteres_passa_e_a_de_121_nao() {
  // "$X," + campos + "*hh": 3 + 114 + 3 = 120 caracteres, sem o \n.
  char corpo[120];
  std::memset(corpo, 'a', sizeof(corpo));
  std::memcpy(corpo, "X,", 2);
  corpo[2 + 114] = '\0';
  char linha[200];
  moldura(corpo, linha, sizeof(linha));
  TEST_ASSERT_EQUAL_size_t(tamanho_maximo + 1, std::strlen(linha));
  Leitor leitor;
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, linha));
  // Com o \r, continua valendo: ele não conta nos 120.
  linha[tamanho_maximo] = '\r';
  linha[tamanho_maximo + 1] = '\n';
  linha[tamanho_maximo + 2] = '\0';
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, linha));

  corpo[2 + 114] = 'a';
  corpo[2 + 115] = '\0';
  moldura(corpo, linha, sizeof(linha));
  TEST_ASSERT_EQUAL_INT(0, aceitas(leitor, linha));
}

static void teste_volta_a_ler_depois_de_linha_longa_ou_errada() {
  char longa[400];
  std::memset(longa, 'a', sizeof(longa));
  longa[0] = '$';
  longa[sizeof(longa) - 2] = '\n';
  longa[sizeof(longa) - 1] = '\0';
  Leitor leitor;
  TEST_ASSERT_EQUAL_INT(0, aceitas(leitor, longa));
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, "$H,1793980812437*52\n"));
  TEST_ASSERT_EQUAL_INT(0, aceitas(leitor, "lixo da partida\n"));
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, "$H,1793980812437*52\n"));
}

static void teste_linha_cortada_nao_vaza_para_a_seguinte() {
  // O começo de uma linha perdida e a linha seguinte inteira formam uma linha só, que é
  // recusada; a próxima volta a valer.
  Leitor leitor;
  TEST_ASSERT_EQUAL_INT(0, aceitas(leitor, "$H,179$H,1793980812437*52\n"));
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, "$H,1793980812437*52\n"));
}

static void teste_tipo_sem_campos() {
  Leitor leitor;
  TEST_ASSERT_EQUAL_INT(1, aceitas(leitor, "$K*4B\n"));
  TEST_ASSERT_EQUAL_CHAR('K', leitor.tipo());
  TEST_ASSERT_EQUAL_STRING("", leitor.campos());
}

static void teste_tipo_desconhecido_e_ignorado() {
  TEST_ASSERT_EQUAL(Comando::Tipo::nenhum, ler("Z,1,2,3").tipo);
  TEST_ASSERT_EQUAL(Comando::Tipo::nenhum, ler("K").tipo);
}

static void teste_linha_da_pi_nao_tem_seq() {
  // O primeiro campo do $C é o nome do parâmetro, não uma seq.
  const Comando comando = ler("C,obd_hz,15");
  TEST_ASSERT_EQUAL(Comando::Tipo::taxa_obd, comando.tipo);
}

// --- $H ---

static void teste_hora_a_partir_de_2026() {
  const Comando piso = ler("H,1767225600000");
  TEST_ASSERT_EQUAL(Comando::Tipo::hora, piso.tipo);
  TEST_ASSERT_EQUAL_UINT64(hora_minima_ms, piso.unix_ms);
  TEST_ASSERT_EQUAL(Comando::Tipo::hora_invalida, ler("H,1767225599999").tipo);
}

static void teste_hora_invalida() {
  const char *invalidas[] = {
      "H",                     // sem campo
      "H,",                    // campo vazio
      "H,0",                   // 1970
      "H,abc",                 // não é número
      "H,-1793980812437",      // negativa
      "H,1793980812437.5",     // com casa decimal
      "H,1793980812437,5",     // campo a mais
      "H,18446744073709551616" // não cabe em 64 bits
  };
  for (const char *corpo : invalidas) {
    TEST_ASSERT_EQUAL_MESSAGE(Comando::Tipo::hora_invalida, ler(corpo).tipo, corpo);
  }
}

// --- $C ---

static void teste_taxa_do_obd_valida() {
  struct Caso {
    const char *corpo;
    uint16_t decimos;
  };
  const Caso casos[] = {
      {"C,obd_hz,15", 150}, {"C,obd_hz,12.5", 125}, {"C,obd_hz,1", 10},
      {"C,obd_hz,20", 200}, {"C,obd_hz,20.0", 200}, {"C,obd_hz,15.0", 150},
      {"C,obd_hz,05", 50},
  };
  for (const Caso &caso : casos) {
    const Comando comando = ler(caso.corpo);
    TEST_ASSERT_EQUAL_MESSAGE(Comando::Tipo::taxa_obd, comando.tipo, caso.corpo);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(caso.decimos, comando.obd_hz_decimos, caso.corpo);
  }
}

static void teste_taxa_do_obd_invalida() {
  const char *invalidas[] = {
      "C,obd_hz,25",    // acima de 20
      "C,obd_hz,20.1",  // acima de 20
      "C,obd_hz,0.5",   // abaixo de 1
      "C,obd_hz,0",     // abaixo de 1
      "C,obd_hz,12.55", // 2 casas
      "C,obd_hz,12.",   // ponto sem casa
      "C,obd_hz,.5",    // sem a parte inteira
      "C,obd_hz,-5",    // negativa
      "C,obd_hz,abc",   // não é número
      "C,obd_hz,",      // sem valor
      "C,obd_hz",       // sem valor
      "C,obd_hz,15,1",  // campo a mais
      "C,outra,5",      // parâmetro desconhecido
      "C",              // sem nada
  };
  for (const char *corpo : invalidas) {
    TEST_ASSERT_EQUAL_MESSAGE(Comando::Tipo::taxa_invalida, ler(corpo).tipo, corpo);
  }
}

// --- Relógio ---

static void teste_relogio_sem_hora_da_t_zero() {
  Relogio relogio;
  TEST_ASSERT_FALSE(relogio.acertado());
  TEST_ASSERT_EQUAL_UINT64(0, relogio.t(12345));
}

static void teste_relogio_acertado_conta_a_partir_da_hora() {
  Relogio relogio;
  relogio.acertar(1793980812437ULL, 5000);
  TEST_ASSERT_TRUE(relogio.acertado());
  TEST_ASSERT_EQUAL_UINT64(1793980812437ULL, relogio.t(5000));
  TEST_ASSERT_EQUAL_UINT64(1793980813437ULL, relogio.t(6000));
}

static void teste_relogio_acerta_de_novo_a_cada_hora() {
  Relogio relogio;
  relogio.acertar(1793980812437ULL, 5000);
  // 60 s depois, o relógio do ESP32 está 3 ms adiantado; o $H seguinte o corrige.
  relogio.acertar(1793980872434ULL, 65000);
  TEST_ASSERT_EQUAL_UINT64(1793980872434ULL, relogio.t(65000));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(teste_hora_do_exemplo_da_proposta);
  RUN_TEST(teste_aceita_cr_antes_do_lf);
  RUN_TEST(teste_recusa_moldura_errada);
  RUN_TEST(teste_linha_de_120_caracteres_passa_e_a_de_121_nao);
  RUN_TEST(teste_volta_a_ler_depois_de_linha_longa_ou_errada);
  RUN_TEST(teste_linha_cortada_nao_vaza_para_a_seguinte);
  RUN_TEST(teste_tipo_sem_campos);
  RUN_TEST(teste_tipo_desconhecido_e_ignorado);
  RUN_TEST(teste_linha_da_pi_nao_tem_seq);
  RUN_TEST(teste_hora_a_partir_de_2026);
  RUN_TEST(teste_hora_invalida);
  RUN_TEST(teste_taxa_do_obd_valida);
  RUN_TEST(teste_taxa_do_obd_invalida);
  RUN_TEST(teste_relogio_sem_hora_da_t_zero);
  RUN_TEST(teste_relogio_acertado_conta_a_partir_da_hora);
  RUN_TEST(teste_relogio_acerta_de_novo_a_cada_hora);
  return UNITY_END();
}
