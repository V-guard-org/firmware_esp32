// Testes do acelerômetro simulado (lib/nucleo/imu_simulado) e da aceleração da viagem: a leitura
// crua que o ADXL345 daria no carro simulado, passando pelo tratamento do núcleo como a do
// sensor (proposta §4.7, #19).
#include <unity.h>

#include <cmath>

#include "central_simulada.h"
#include "imu_simulado.h"

using namespace vguard;

namespace {

constexpr float ms2_por_lsb = imu::g_por_lsb * imu::gravidade;

imu::Leitura leitura_da_viagem(uint32_t ms) {
  const obd::Viagem v = obd::viagem(ms);
  return imu::leitura_simulada(v.aceleracao, v.rpm, ms);
}

// A maior distância de cada eixo, em m/s², entre as leituras de um trecho e o valor sem vibração.
float maior_vibracao(float aceleracao, float rpm, uint32_t inicio) {
  float maior = 0;
  for (uint32_t ms = inicio; ms < inicio + 2000; ms += 10) {
    const imu::Leitura l = imu::leitura_simulada(aceleracao, rpm, ms);
    const float dx = std::fabs(l.x * ms2_por_lsb - aceleracao);
    const float dy = std::fabs(l.y * ms2_por_lsb);
    const float dz = std::fabs(l.z * ms2_por_lsb - imu::gravidade);
    maior = std::fmax(maior, std::fmax(dx, std::fmax(dy, dz)));
  }
  return maior;
}

}  // namespace

void setUp() {}

void tearDown() {}

static void teste_aceleracao_da_viagem() {
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0, obd::viagem(10000).aceleracao);    // parado
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.111f, obd::viagem(40000).aceleracao);  // 0 a 80 em 20 s
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0, obd::viagem(52000).aceleracao);    // 80 a 80
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0, obd::viagem(100000).aceleracao);   // cruzeiro
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -1.111f, obd::viagem(210000).aceleracao);  // freando
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0, obd::viagem(250000).aceleracao);   // parado
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.111f, obd::viagem(340000).aceleracao);  // 2ª volta
}

static void teste_leitura_parada_e_1_g_no_z() {
  // A média de 1 s parado: x e y perto de 0 e z perto de 256 LSB (1 g).
  long soma[3] = {};
  for (uint32_t ms = 10000; ms < 11000; ms += 10) {
    const imu::Leitura l = leitura_da_viagem(ms);
    soma[0] += l.x;
    soma[1] += l.y;
    soma[2] += l.z;
  }
  TEST_ASSERT_INT_WITHIN(1, 0, soma[0] / 100);
  TEST_ASSERT_INT_WITHIN(1, 0, soma[1] / 100);
  TEST_ASSERT_INT_WITHIN(1, 256, soma[2] / 100);
}

static void teste_vibracao_cresce_com_o_rpm() {
  const float lenta = maior_vibracao(0, 800, 0);
  const float alta = maior_vibracao(0, 3000, 0);
  TEST_ASSERT_TRUE(lenta <= imu::vibracao_minima + ms2_por_lsb);
  TEST_ASSERT_TRUE(alta <= imu::vibracao_maxima + ms2_por_lsb);
  TEST_ASSERT_TRUE(alta > 0.2f);
  TEST_ASSERT_TRUE(alta > 3 * lenta);
}

static void teste_mesma_leitura_no_mesmo_instante() {
  const imu::Leitura a = leitura_da_viagem(123450);
  const imu::Leitura b = leitura_da_viagem(123450);
  const imu::Leitura c = leitura_da_viagem(123460);
  TEST_ASSERT_TRUE(a.x == b.x && a.y == b.y && a.z == b.z);
  TEST_ASSERT_FALSE(a.x == c.x && a.y == c.y && a.z == c.z);
}

static void teste_passa_na_conferencia_de_1g() {
  imu::Leitura leituras[imu::leituras_conferencia];
  for (uint32_t i = 0; i < imu::leituras_conferencia; ++i) {
    leituras[i] = leitura_da_viagem(i * 10);
  }
  TEST_ASSERT_TRUE(imu::conferir_1g(leituras, imu::leituras_conferencia, {}));
}

static void teste_a_pelo_tratamento_acelerando() {
  // De 38 a 42 s, a 100 Hz, pelo filtro e 1 de cada 2: o $A mostra a aceleração.
  imu::Tratamento tratamento;
  int saidas = 0;
  for (uint32_t ms = 38000; ms < 42000; ms += 10) {
    imu::Aceleracao a;
    if (tratamento.receber(leitura_da_viagem(ms), a) && ms >= 39000) {
      TEST_ASSERT_FLOAT_WITHIN(0.15f, 1.111f, a.x);
      TEST_ASSERT_FLOAT_WITHIN(0.15f, 0, a.y);
      TEST_ASSERT_FLOAT_WITHIN(0.15f, 9.79f, a.z);
      ++saidas;
    }
  }
  TEST_ASSERT_EQUAL_INT(150, saidas);
}

static void teste_a_pelo_tratamento_freando() {
  imu::Tratamento tratamento;
  for (uint32_t ms = 205000; ms < 215000; ms += 10) {
    imu::Aceleracao a;
    if (tratamento.receber(leitura_da_viagem(ms), a) && ms >= 206000) {
      TEST_ASSERT_FLOAT_WITHIN(0.15f, -1.111f, a.x);
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(teste_aceleracao_da_viagem);
  RUN_TEST(teste_leitura_parada_e_1_g_no_z);
  RUN_TEST(teste_vibracao_cresce_com_o_rpm);
  RUN_TEST(teste_mesma_leitura_no_mesmo_instante);
  RUN_TEST(teste_passa_na_conferencia_de_1g);
  RUN_TEST(teste_a_pelo_tratamento_acelerando);
  RUN_TEST(teste_a_pelo_tratamento_freando);
  return UNITY_END();
}
