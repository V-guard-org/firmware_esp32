// Testes do log no microSD (lib/nucleo/arquivo_sd), com um cartão de mentira na RAM: o nome do
// arquivo pela hora UTC, o cabeçalho, o $B guardado até a hora e a troca de arquivo na virada
// (proposta §4.5).
#include <unity.h>

#include <cstdio>
#include <cstring>

#include "arquivo_sd.h"

using namespace vguard::arquivo_sd;
using vguard::protocolo::checksum;

namespace {

constexpr const char *versao = "0.7.0-1a2b3c4";
constexpr const char *cabecalho =
    "# V-GUARD 0.7.0-1a2b3c4 | B,seq,t,firmware,motivo_reset | "
    "S,seq,t,uptime_s,estado,obd,imu,sd,sd_livre,obd_hz,descartadas | A,seq,t,ax,ay,az | "
    "O,seq,t,campo,valor | P,seq,t,mapa00,mapa20,mapa40 | L,seq,t,nivel,texto | H,unix_ms\n";

// Guarda os arquivos na RAM. Abrir um arquivo que já existe continua no fim dele, como o
// FILE_APPEND do SD.
class CartaoDeTeste final : public Cartao {
 public:
  bool abrir(const char *nome) override {
    ++aberturas;
    if (falhar_ao_abrir) {
      return false;
    }
    for (size_t i = 0; i < quantidade; ++i) {
      if (std::strcmp(arquivos[i].nome, nome) == 0) {
        atual = &arquivos[i];
        return true;
      }
    }
    if (quantidade == maximo_arquivos) {
      return false;
    }
    atual = &arquivos[quantidade++];
    std::strncpy(atual->nome, nome, sizeof(atual->nome) - 1);
    return true;
  }

  bool escrever(const char *texto, size_t tamanho) override {
    ++escritas;
    if (falhar_ao_escrever || atual == nullptr ||
        atual->tamanho + tamanho >= sizeof(atual->conteudo)) {
      return false;
    }
    std::memcpy(atual->conteudo + atual->tamanho, texto, tamanho);
    atual->tamanho += tamanho;
    return true;
  }

  struct Arquivo {
    char nome[tamanho_nome] = {};
    char conteudo[4096] = {};
    size_t tamanho = 0;
  };

  // O conteúdo do arquivo, ou "" se ele não existe.
  const char *conteudo(const char *nome) const {
    for (size_t i = 0; i < quantidade; ++i) {
      if (std::strcmp(arquivos[i].nome, nome) == 0) {
        return arquivos[i].conteudo;
      }
    }
    return "";
  }

  static constexpr size_t maximo_arquivos = 4;
  Arquivo arquivos[maximo_arquivos];
  size_t quantidade = 0;
  Arquivo *atual = nullptr;
  int aberturas = 0;
  int escritas = 0;
  bool falhar_ao_abrir = false;
  bool falhar_ao_escrever = false;
};

// A linha com a moldura, como sai na serial: "A,57,..." vira "$A,57,...*hh\n".
struct ComMoldura {
  explicit ComMoldura(const char *texto) {
    const size_t n = std::strlen(texto);
    tamanho = static_cast<size_t>(
        std::snprintf(linha, sizeof(linha), "$%s*%02X\n", texto, checksum(texto, n)));
  }
  char linha[160];
  size_t tamanho;
};

void receber(Registro &registro, const char *texto) {
  const ComMoldura com_moldura(texto);
  registro.receber(com_moldura.linha, com_moldura.tamanho);
}

// O cabeçalho seguido das linhas.
void conferir(const char *esperado_sem_cabecalho, const char *conteudo) {
  char esperado[2048];
  std::snprintf(esperado, sizeof(esperado), "%s%s", cabecalho, esperado_sem_cabecalho);
  TEST_ASSERT_EQUAL_STRING(esperado, conteudo);
}

void conferir_nome(uint64_t t, const char *esperado) {
  char nome[tamanho_nome];
  nome_do_arquivo(t, nome);
  TEST_ASSERT_EQUAL_STRING(esperado, nome);
}

// 08/11/2026 00:58:13 UTC.
constexpr uint64_t hora_do_teste = 1794099493000ULL;
// 08/11/2026 01:00:00 UTC, a virada seguinte.
constexpr uint64_t virada = 1794099600000ULL;

}  // namespace

void setUp() {}

void tearDown() {}

static void teste_nome_pela_hora_utc() {
  conferir_nome(hora_do_teste, "20261108-00.csv");
  conferir_nome(1767225600000ULL, "20260101-00.csv");  // o piso do $H
  conferir_nome(1793980812437ULL, "20261106-16.csv");  // o exemplo da proposta §5.6
  conferir_nome(1830297599999ULL, "20271231-23.csv");  // o último ms do ano
  conferir_nome(1830297600000ULL, "20280101-00.csv");
  conferir_nome(1835481540000ULL, "20280229-23.csv");  // ano bissexto
  conferir_nome(4102444799999ULL, "20991231-23.csv");
}

static void teste_nada_antes_do_primeiro_h() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "S,1,0,0,ESPERA,PARADO,FALHA,OK,97,12.5,0");
  receber(registro, "L,2,0,ERRO,IMU sem resposta");
  receber(registro, "S,3,0,1,ESPERA,PARADO,FALHA,OK,97,12.5,0");
  TEST_ASSERT_EQUAL_INT(0, cartao.aberturas);
  TEST_ASSERT_EQUAL_INT(0, cartao.escritas);
}

static void teste_boot_guardado_ate_a_hora() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "B,0,0,0.7.0-1a2b3c4,LIGOU");
  receber(registro, "S,1,0,0,ESPERA,PARADO,OK,OK,97,12.5,0");
  TEST_ASSERT_EQUAL_INT(0, cartao.aberturas);
  receber(registro, "H,1794099493000");
  receber(registro, "S,2,1794099493312,1,ATIVO,ESCUTANDO,OK,OK,97,12.5,0");
  receber(registro, "A,3,1794099493320,0.12,-0.05,9.79");
  TEST_ASSERT_EQUAL_UINT(1, cartao.quantidade);
  conferir(
      "B,0,0,0.7.0-1a2b3c4,LIGOU\n"
      "H,1794099493000\n"
      "S,2,1794099493312,1,ATIVO,ESCUTANDO,OK,OK,97,12.5,0\n"
      "A,3,1794099493320,0.12,-0.05,9.79\n",
      cartao.conteudo("20261108-00.csv"));
  TEST_ASSERT_FALSE(registro.falhou());
}

static void teste_sem_boot_comeca_pelo_h() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "H,1794099493000");
  conferir("H,1794099493000\n", cartao.conteudo("20261108-00.csv"));
}

static void teste_todos_os_tipos_sem_moldura() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "H,1794099493000");
  receber(registro, "P,4,1794099493400,183A8001,00000001,40000000");
  receber(registro, "O,5,1794099493420,rpm,876.25");
  receber(registro, "L,6,1794099493500,AVISO,PID 0F sem resposta");
  receber(registro, "H,1794099553000");
  conferir(
      "H,1794099493000\n"
      "P,4,1794099493400,183A8001,00000001,40000000\n"
      "O,5,1794099493420,rpm,876.25\n"
      "L,6,1794099493500,AVISO,PID 0F sem resposta\n"
      "H,1794099553000\n",
      cartao.conteudo("20261108-00.csv"));
}

static void teste_arquivo_novo_na_virada_da_hora() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "H,1794099590000");                 // 00:59:50
  receber(registro, "A,10,1794099599980,0,0,9.81");    // 00:59:59,980
  receber(registro, "A,11,1794099600000,0,0,9.81");    // 01:00:00,000
  receber(registro, "A,12,1794099600020,0,0,9.81");
  TEST_ASSERT_EQUAL_UINT(2, cartao.quantidade);
  conferir("H,1794099590000\nA,10,1794099599980,0,0,9.81\n", cartao.conteudo("20261108-00.csv"));
  conferir("A,11,1794099600000,0,0,9.81\nA,12,1794099600020,0,0,9.81\n",
           cartao.conteudo("20261108-01.csv"));
  TEST_ASSERT_EQUAL_INT(2, cartao.aberturas);
}

static void teste_hora_que_volta_reabre_o_arquivo_anterior() {
  // Um $H que atrasa o relógio bem na virada: a linha volta para o arquivo da hora anterior,
  // depois de um cabeçalho novo.
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "H,1794099599000");
  receber(registro, "A,10,1794099600005,0,0,9.81");
  receber(registro, "H,1794099599995");
  receber(registro, "A,11,1794099600015,0,0,9.81");
  char esperado[1024];
  std::snprintf(esperado, sizeof(esperado), "%sH,1794099599000\n%sH,1794099599995\n", cabecalho,
                cabecalho);
  TEST_ASSERT_EQUAL_STRING(esperado, cartao.conteudo("20261108-00.csv"));
  std::snprintf(esperado, sizeof(esperado),
                "%sA,10,1794099600005,0,0,9.81\n%sA,11,1794099600015,0,0,9.81\n", cabecalho,
                cabecalho);
  TEST_ASSERT_EQUAL_STRING(esperado, cartao.conteudo("20261108-01.csv"));
}

static void teste_reinicio_continua_no_fim_do_arquivo_da_hora() {
  // Dois boots na mesma hora: o segundo acrescenta no fim, depois de um cabeçalho novo.
  CartaoDeTeste cartao;
  {
    Registro primeiro(cartao, versao);
    receber(primeiro, "B,0,0,0.7.0-1a2b3c4,LIGOU");
    receber(primeiro, "H,1794099493000");
    receber(primeiro, "A,5,1794099493320,0.12,-0.05,9.79");
  }
  Registro segundo(cartao, versao);
  receber(segundo, "B,0,0,0.7.0-1a2b3c4,QUEDA_TENSAO");
  receber(segundo, "H,1794099553000");
  char esperado[1024];
  std::snprintf(esperado, sizeof(esperado),
                "%sB,0,0,0.7.0-1a2b3c4,LIGOU\nH,1794099493000\nA,5,1794099493320,0.12,-0.05,9.79\n"
                "%sB,0,0,0.7.0-1a2b3c4,QUEDA_TENSAO\nH,1794099553000\n",
                cabecalho, cabecalho);
  TEST_ASSERT_EQUAL_STRING(esperado, cartao.conteudo("20261108-00.csv"));
}

static void teste_linhas_cortadas_em_qualquer_ponto() {
  // Os bytes chegam da fila em blocos que cortam as linhas; o resultado é o mesmo.
  char fila[512];
  size_t tamanho = 0;
  const char *const textos[] = {"B,0,0,0.7.0-1a2b3c4,LIGOU",
                                "S,1,0,0,ESPERA,PARADO,OK,OK,97,12.5,0", "H,1794099493000",
                                "A,3,1794099493320,0.12,-0.05,9.79"};
  for (const char *texto : textos) {
    const ComMoldura com_moldura(texto);
    std::memcpy(fila + tamanho, com_moldura.linha, com_moldura.tamanho);
    tamanho += com_moldura.tamanho;
  }
  for (size_t bloco = 1; bloco <= 7; ++bloco) {
    CartaoDeTeste cartao;
    Registro registro(cartao, versao);
    for (size_t i = 0; i < tamanho; i += bloco) {
      registro.receber(fila + i, i + bloco <= tamanho ? bloco : tamanho - i);
    }
    conferir("B,0,0,0.7.0-1a2b3c4,LIGOU\nH,1794099493000\nA,3,1794099493320,0.12,-0.05,9.79\n",
             cartao.conteudo("20261108-00.csv"));
  }
}

static void teste_linha_longa_ou_sem_moldura_e_ignorada() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "H,1794099493000");
  char longa[200];
  std::memset(longa, 'x', sizeof(longa) - 2);
  longa[0] = '$';
  longa[sizeof(longa) - 2] = '\n';
  longa[sizeof(longa) - 1] = '\0';
  registro.receber(longa, std::strlen(longa));
  registro.receber("A,4,1794099493320,0,0,9.81\n", 27);  // sem $ e sem *hh
  registro.receber("$A,5\n", 5);
  receber(registro, "A,6,1794099493340,0,0,9.81");
  conferir("H,1794099493000\nA,6,1794099493340,0,0,9.81\n", cartao.conteudo("20261108-00.csv"));
}

static void teste_aceita_cr_antes_do_lf() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  const ComMoldura com_moldura("H,1794099493000");
  char linha[64];
  std::memcpy(linha, com_moldura.linha, com_moldura.tamanho - 1);
  std::memcpy(linha + com_moldura.tamanho - 1, "\r\n", 2);
  registro.receber(linha, com_moldura.tamanho + 1);
  conferir("H,1794099493000\n", cartao.conteudo("20261108-00.csv"));
}

static void teste_erro_ao_abrir_para_de_gravar() {
  CartaoDeTeste cartao;
  cartao.falhar_ao_abrir = true;
  Registro registro(cartao, versao);
  receber(registro, "H,1794099493000");
  TEST_ASSERT_TRUE(registro.falhou());
  cartao.falhar_ao_abrir = false;
  receber(registro, "A,3,1794099493320,0.12,-0.05,9.79");
  receber(registro, "H,1794099553000");
  TEST_ASSERT_EQUAL_INT(1, cartao.aberturas);
  TEST_ASSERT_EQUAL_INT(0, cartao.escritas);
}

static void teste_erro_ao_escrever_para_de_gravar() {
  CartaoDeTeste cartao;
  Registro registro(cartao, versao);
  receber(registro, "H,1794099493000");
  TEST_ASSERT_FALSE(registro.falhou());
  cartao.falhar_ao_escrever = true;
  receber(registro, "A,3,1794099493320,0.12,-0.05,9.79");
  TEST_ASSERT_TRUE(registro.falhou());
  const int escritas = cartao.escritas;
  cartao.falhar_ao_escrever = false;
  receber(registro, "A,4,1794099493340,0.12,-0.05,9.79");
  receber(registro, "A,5,1794099600000,0.12,-0.05,9.79");  // outra hora
  TEST_ASSERT_EQUAL_INT(escritas, cartao.escritas);
  TEST_ASSERT_EQUAL_INT(1, cartao.aberturas);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(teste_nome_pela_hora_utc);
  RUN_TEST(teste_nada_antes_do_primeiro_h);
  RUN_TEST(teste_boot_guardado_ate_a_hora);
  RUN_TEST(teste_sem_boot_comeca_pelo_h);
  RUN_TEST(teste_todos_os_tipos_sem_moldura);
  RUN_TEST(teste_arquivo_novo_na_virada_da_hora);
  RUN_TEST(teste_hora_que_volta_reabre_o_arquivo_anterior);
  RUN_TEST(teste_reinicio_continua_no_fim_do_arquivo_da_hora);
  RUN_TEST(teste_linhas_cortadas_em_qualquer_ponto);
  RUN_TEST(teste_linha_longa_ou_sem_moldura_e_ignorada);
  RUN_TEST(teste_aceita_cr_antes_do_lf);
  RUN_TEST(teste_erro_ao_abrir_para_de_gravar);
  RUN_TEST(teste_erro_ao_escrever_para_de_gravar);
  return UNITY_END();
}
