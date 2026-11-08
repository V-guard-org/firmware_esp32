// Testes do tratamento do acelerômetro (lib/nucleo/imu), proposta §4.4.
#include <unity.h>

#include <cmath>

#include "imu.h"

using namespace vguard::imu;

namespace {

constexpr Aceleracao sem_offset{0.0f, 0.0f, 0.0f};

// 1 LSB em m/s²: 3,9 mg.
constexpr float lsb = g_por_lsb * gravidade;

Leitura parada(int16_t z) { return Leitura{0, 0, z}; }

}  // namespace

void setUp() {}

void tearDown() {}

static void teste_alfa_corta_em_5_hz() {
  const double esperado = 1.0 - std::exp(-2.0 * M_PI * 5.0 / 100.0);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, static_cast<float>(esperado), alfa);
}

static void teste_conversao_para_ms2() {
  // Deitada: 256 LSB no z, perto de 1 g.
  const Aceleracao a = em_ms2(Leitura{0, -13, 256}, sem_offset);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, a.x);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -13 * lsb, a.y);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 9.791f, a.z);
  // De cabeça para baixo.
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -9.791f, em_ms2(parada(-256), sem_offset).z);
}

static void teste_offset_e_subtraido() {
  const Aceleracao offset{0.5f, -0.25f, 1.0f};
  const Aceleracao a = em_ms2(Leitura{0, 0, 256}, offset);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -0.5f, a.x);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.25f, a.y);
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 8.791f, a.z);
}

static void teste_centesimos_arredonda_para_o_mais_proximo() {
  TEST_ASSERT_EQUAL_INT32(979, centesimos(9.786f));
  TEST_ASSERT_EQUAL_INT32(-5, centesimos(-0.054f));
  TEST_ASSERT_EQUAL_INT32(0, centesimos(0.004f));
  // Metade: para longe do zero, nos dois sinais.
  TEST_ASSERT_EQUAL_INT32(13, centesimos(0.125f));
  TEST_ASSERT_EQUAL_INT32(-13, centesimos(-0.125f));
}

static void teste_conferencia_aceita_de_0_8_a_1_2_g() {
  // 205 LSB dão 0,7995 g; 206, 0,8034 g; 307, 1,1973 g; 308, 1,2012 g.
  const int16_t dentro[] = {206, 256, 307, -256};
  for (int16_t z : dentro) {
    const Leitura leituras[] = {parada(z), parada(z)};
    TEST_ASSERT_TRUE_MESSAGE(conferir_1g(leituras, 2, sem_offset), "deveria aceitar");
  }
  const int16_t fora[] = {0, 205, 308, -400};
  for (int16_t z : fora) {
    const Leitura leituras[] = {parada(z), parada(z)};
    TEST_ASSERT_FALSE_MESSAGE(conferir_1g(leituras, 2, sem_offset), "deveria recusar");
  }
}

static void teste_conferencia_vale_em_qualquer_inclinacao() {
  // 45° entre o x e o z: 181 LSB em cada, |a| perto de 1 g.
  const Leitura inclinada[] = {Leitura{181, 0, 181}};
  TEST_ASSERT_TRUE(conferir_1g(inclinada, 1, sem_offset));
}

static void teste_conferencia_usa_a_media() {
  // Cada leitura sozinha está fora (0 e 2 g); a média, 1 g, está dentro.
  const Leitura leituras[] = {parada(0), parada(512)};
  TEST_ASSERT_TRUE(conferir_1g(leituras, 2, sem_offset));
}

static void teste_conferencia_desconta_o_offset() {
  const Leitura leituras[] = {parada(256)};
  const Aceleracao offset_que_tira_tudo{0.0f, 0.0f, 256 * lsb};
  TEST_ASSERT_FALSE(conferir_1g(leituras, 1, offset_que_tira_tudo));
}

static void teste_conferencia_sem_leituras_nao_passa() {
  TEST_ASSERT_FALSE(conferir_1g(nullptr, 0, sem_offset));
  const Leitura leituras[] = {parada(256)};
  TEST_ASSERT_FALSE(conferir_1g(leituras, 0, sem_offset));
}

static void teste_sai_uma_de_cada_duas() {
  Tratamento tratamento;
  Aceleracao saida{};
  int saidas = 0;
  for (int i = 0; i < 100; ++i) {
    const bool saiu = tratamento.receber(parada(256), saida);
    // A primeira sai, a segunda não, e assim por diante.
    TEST_ASSERT_EQUAL(i % 2 == 0, saiu);
    saidas += saiu ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_INT(50, saidas);
}

static void teste_primeira_leitura_entra_direto_sem_subir_de_0() {
  Tratamento tratamento;
  Aceleracao saida{};
  TEST_ASSERT_TRUE(tratamento.receber(Leitura{26, -13, 256}, saida));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 26 * lsb, saida.x);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, -13 * lsb, saida.y);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 256 * lsb, saida.z);
}

static void teste_degrau_segue_o_filtro() {
  Tratamento tratamento;
  Aceleracao saida{};
  TEST_ASSERT_TRUE(tratamento.receber(parada(0), saida));
  // Degrau de 0 para 256 LSB: depois de k leituras, y = 256·(1 − (1 − α)^k).
  TEST_ASSERT_FALSE(tratamento.receber(parada(256), saida));
  TEST_ASSERT_TRUE(tratamento.receber(parada(256), saida));
  const float depois_de_2 = 256 * lsb * (1.0f - (1.0f - alfa) * (1.0f - alfa));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, depois_de_2, saida.z);
  // Uns 0,2 s depois (20 leituras), já chegou.
  for (int i = 0; i < 20; ++i) {
    tratamento.receber(parada(256), saida);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 256 * lsb, saida.z);
}

static void teste_vibracao_rapida_cai() {
  // ±100 LSB a 50 Hz em volta de 1 g. Na saída, sobra α/(2 − α) ≈ 16% da vibração.
  Tratamento tratamento;
  Aceleracao saida{};
  for (int i = 0; i < 200; ++i) {
    tratamento.receber(parada(static_cast<int16_t>(i % 2 == 0 ? 356 : 156)), saida);
  }
  const float sobra = std::fabs(saida.z - 256 * lsb) / (100 * lsb);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, alfa / (2.0f - alfa), sobra);
}

static void teste_recomecar_esquece_o_filtro() {
  Tratamento tratamento;
  Aceleracao saida{};
  tratamento.receber(parada(256), saida);
  tratamento.receber(parada(256), saida);
  tratamento.receber(parada(256), saida);
  tratamento.recomecar();
  // Depois da volta do sensor, a primeira leitura sai direto, sem o valor de antes.
  TEST_ASSERT_TRUE(tratamento.receber(parada(-256), saida));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, -256 * lsb, saida.z);
}

static void teste_offset_vale_no_tratamento() {
  Tratamento tratamento;
  tratamento.definir_offset(Aceleracao{0.0f, 0.0f, 0.5f});
  Aceleracao saida{};
  TEST_ASSERT_TRUE(tratamento.receber(parada(256), saida));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, 256 * lsb - 0.5f, saida.z);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(teste_alfa_corta_em_5_hz);
  RUN_TEST(teste_conversao_para_ms2);
  RUN_TEST(teste_offset_e_subtraido);
  RUN_TEST(teste_centesimos_arredonda_para_o_mais_proximo);
  RUN_TEST(teste_conferencia_aceita_de_0_8_a_1_2_g);
  RUN_TEST(teste_conferencia_vale_em_qualquer_inclinacao);
  RUN_TEST(teste_conferencia_usa_a_media);
  RUN_TEST(teste_conferencia_desconta_o_offset);
  RUN_TEST(teste_conferencia_sem_leituras_nao_passa);
  RUN_TEST(teste_sai_uma_de_cada_duas);
  RUN_TEST(teste_primeira_leitura_entra_direto_sem_subir_de_0);
  RUN_TEST(teste_degrau_segue_o_filtro);
  RUN_TEST(teste_vibracao_rapida_cai);
  RUN_TEST(teste_recomecar_esquece_o_filtro);
  RUN_TEST(teste_offset_vale_no_tratamento);
  return UNITY_END();
}
