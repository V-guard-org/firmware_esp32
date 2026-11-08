// Testes da central simulada (lib/nucleo/central_simulada), a do ambiente esp32-sim: os mapas, a
// viagem em laço e a máquina do OBD lendo dela, como no ESP32 (proposta §4.7, #17).
#include <unity.h>

#include <cstring>

#include "central_simulada.h"

using namespace vguard::obd;
using vguard::protocolo::EstadoObd;

namespace {

Quadro pedir(uint16_t id, uint8_t pid, uint32_t agora, bool &respondeu) {
  const uint8_t pedido[8] = {2, 1, pid, 0, 0, 0, 0, 0};
  Quadro resposta{};
  respondeu = responder(id, pedido, agora, resposta);
  return resposta;
}

// O valor que a máquina leria da resposta, pelo decodificar() do núcleo.
float ler(uint8_t pid, uint32_t agora) {
  bool respondeu = false;
  const Quadro resposta = pedir(id_central, pid, agora, respondeu);
  TEST_ASSERT_TRUE(respondeu);
  uint8_t bytes = 0;
  TEST_ASSERT_TRUE(resposta_positiva(resposta.id, resposta.dlc, resposta.dados, id_resposta, pid,
                                     bytes));
  float valor = 0;
  TEST_ASSERT_TRUE(decodificar(pid, resposta.dados + 3, bytes, valor));
  return valor;
}

// O firmware como no ESP32: a resposta da central chega no ciclo seguinte (10 ms), e um quadro
// de fundo a cada 20 ms.
class SaidaParaACentral final : public Saida {
 public:
  bool configurar_leitura() override { return true; }
  bool voltar_a_escutar() override { return true; }
  void enviar(uint16_t id, const uint8_t (&dados)[8]) override {
    tem_resposta = responder(id, dados, agora, resposta);
  }
  void emitir_obd(const char *campo, int64_t valor, uint8_t casas) override {
    static_cast<void>(casas);
    for (size_t i = 0; i < quantidade_campos; ++i) {
      if (std::strcmp(campos[i].nome, campo) == 0) {
        ultimo_valor[i] = valor;
        ++leituras[i];
      }
    }
  }
  void emitir_pids(uint32_t mapa00, uint32_t mapa20, uint32_t mapa40) override {
    pids[0] = mapa00;
    pids[1] = mapa20;
    pids[2] = mapa40;
    ++pids_emitidos;
  }
  void emitir_aviso(const char *) override { ++avisos; }

  uint32_t agora = 0;
  bool tem_resposta = false;
  Quadro resposta{};
  int64_t ultimo_valor[quantidade_campos] = {};
  int leituras[quantidade_campos] = {};
  uint32_t pids[3] = {};
  int pids_emitidos = 0;
  int avisos = 0;
};

void rodar(Maquina &maquina, SaidaParaACentral &saida, uint32_t ate) {
  const Quadro fundo{0x3A0, 8, {}};
  for (; saida.agora < ate; saida.agora += 10) {
    if (saida.tem_resposta) {
      saida.tem_resposta = false;
      maquina.receber(saida.resposta, saida.agora);
    }
    if (saida.agora % 20 == 0) {
      maquina.receber(fundo, saida.agora);
    }
    maquina.tique(saida.agora, 125);
  }
}

}  // namespace

void setUp() {}

void tearDown() {}

static void teste_mapas_do_voyage() {
  bool respondeu = false;
  const uint8_t mapa00[8] = {0x06, 0x41, 0x00, 0x18, 0x3A, 0x80, 0x01, 0x55};
  const uint8_t mapa20[8] = {0x06, 0x41, 0x20, 0x00, 0x00, 0x00, 0x01, 0x55};
  const uint8_t mapa40[8] = {0x06, 0x41, 0x40, 0x40, 0x00, 0x00, 0x00, 0x55};
  Quadro resposta = pedir(id_todos, 0x00, 0, respondeu);
  TEST_ASSERT_TRUE(respondeu);
  TEST_ASSERT_EQUAL_HEX32(id_resposta, resposta.id);
  TEST_ASSERT_EQUAL_UINT8(8, resposta.dlc);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(mapa00, resposta.dados, 8);
  resposta = pedir(id_central, 0x20, 0, respondeu);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(mapa20, resposta.dados, 8);
  resposta = pedir(id_central, 0x40, 0, respondeu);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(mapa40, resposta.dados, 8);
}

static void teste_formato_das_respostas() {
  bool respondeu = false;
  // rpm a 100 s: 2200 → 8800 = 0x2260, em 2 bytes.
  const uint8_t rpm[8] = {0x04, 0x41, 0x0C, 0x22, 0x60, 0x55, 0x55, 0x55};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(rpm, pedir(id_central, 0x0C, 100000, respondeu).dados, 8);
  // speed a 100 s: 80 km/h, em 1 byte.
  const uint8_t speed[8] = {0x03, 0x41, 0x0D, 0x50, 0x55, 0x55, 0x55, 0x55};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(speed, pedir(id_central, 0x0D, 100000, respondeu).dados, 8);
}

static void teste_pedido_que_a_central_nao_responde() {
  bool respondeu = true;
  pedir(id_central, 0x01, 0, respondeu);  // PID que ela não tem
  TEST_ASSERT_FALSE(respondeu);
  pedir(0x7E1, 0x0C, 0, respondeu);  // outro ID
  TEST_ASSERT_FALSE(respondeu);
  const uint8_t servico_09[8] = {2, 9, 2, 0, 0, 0, 0, 0};
  Quadro resposta{};
  TEST_ASSERT_FALSE(responder(id_central, servico_09, 0, resposta));
}

static void teste_viagem_nos_pontos_do_perfil() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0, viagem(0).speed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 800, viagem(0).rpm);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 800, viagem(30000).rpm);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 80, viagem(50000).speed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 3000, viagem(50000).rpm);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2200, viagem(100000).rpm);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 25, viagem(100000).throttle);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 0, viagem(220000).speed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 15, viagem(220000).load);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 20, viagem(299990).load);
}

static void teste_viagem_interpola_entre_os_pontos() {
  // No meio da aceleração (40 s): metade do caminho de 0 a 80 km/h e de 800 a 3000 rpm.
  const Viagem meio = viagem(40000);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40, meio.speed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1900, meio.rpm);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 32.5f, meio.throttle);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 47.5f, meio.load);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 57.5f, meio.map);
  // No meio da frenagem (210 s).
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40, viagem(210000).speed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1500, viagem(210000).rpm);
}

static void teste_viagem_recomeca_a_cada_300_s() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, viagem(40000).speed, viagem(340000).speed);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, viagem(40000).rpm, viagem(940000).rpm);
}

static void teste_coolant_aquece_em_10_min() {
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 47, viagem(0).coolant);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 68.5f, viagem(300000).coolant);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90, viagem(600000).coolant);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 90, viagem(3600000).coolant);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30, viagem(0).iat);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.8f, viagem(0).voltage);
}

static void teste_valores_decodificados_pela_maquina() {
  // A 40 s: o que o $O mostraria.
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 1900, ler(0x0C, 40000));
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 40, ler(0x0D, 40000));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 32.5f, ler(0x11, 40000));  // em passos de 100/255 %
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 47.5f, ler(0x04, 40000));
  TEST_ASSERT_FLOAT_WITHIN(1, 57.5f, ler(0x0B, 40000));
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 49.87f, ler(0x05, 40000));  // 47 + 43 · 40/600, inteiro
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30, ler(0x0F, 40000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.8f, ler(0x42, 40000));
}

static void teste_maquina_le_os_8_da_central() {
  SaidaParaACentral saida;
  Maquina maquina(saida);
  maquina.comecar(0);
  rodar(maquina, saida, 60000);
  TEST_ASSERT_EQUAL_INT(1, saida.pids_emitidos);
  TEST_ASSERT_EQUAL_HEX32(0x183A8001UL, saida.pids[0]);
  TEST_ASSERT_EQUAL_HEX32(0x00000001UL, saida.pids[1]);
  TEST_ASSERT_EQUAL_HEX32(0x40000000UL, saida.pids[2]);
  TEST_ASSERT_EQUAL(EstadoObd::lendo, maquina.estado());
  TEST_ASSERT_EQUAL_INT(0, saida.avisos);
  // ~12,5 por segundo, divididos pelos 8: ~94 de cada em 60 s.
  for (size_t i = 0; i < quantidade_campos; ++i) {
    TEST_ASSERT_INT_WITHIN(2, 93, saida.leituras[i]);
  }
  // Aos 60 s, em cruzeiro: speed 80 (inteiro) e rpm 2200 (2 casas).
  TEST_ASSERT_EQUAL_INT64(80, saida.ultimo_valor[4]);
  TEST_ASSERT_EQUAL_INT64(220000, saida.ultimo_valor[3]);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(teste_mapas_do_voyage);
  RUN_TEST(teste_formato_das_respostas);
  RUN_TEST(teste_pedido_que_a_central_nao_responde);
  RUN_TEST(teste_viagem_nos_pontos_do_perfil);
  RUN_TEST(teste_viagem_interpola_entre_os_pontos);
  RUN_TEST(teste_viagem_recomeca_a_cada_300_s);
  RUN_TEST(teste_coolant_aquece_em_10_min);
  RUN_TEST(teste_valores_decodificados_pela_maquina);
  RUN_TEST(teste_maquina_le_os_8_da_central);
  return UNITY_END();
}
