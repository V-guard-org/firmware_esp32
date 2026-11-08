#include "imu_simulado.h"

#include <cmath>

namespace vguard::imu {

namespace {

constexpr float rpm_minimo = 800;
constexpr float rpm_maximo = 3000;
constexpr float ms2_por_lsb = g_por_lsb * gravidade;

// Um número de −1 a 1, sempre o mesmo para o mesmo instante e eixo: um xorshift de 32 bits
// sobre o instante misturado com o eixo.
float ruido(uint32_t ms, uint32_t eixo) {
  uint32_t x = ms * 2654435761U + (eixo + 1) * 0x9E3779B9U;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return static_cast<float>(x) / 4294967295.0f * 2 - 1;
}

int16_t em_lsb(float ms2) { return static_cast<int16_t>(std::lround(ms2 / ms2_por_lsb)); }

}  // namespace

Leitura leitura_simulada(float aceleracao, float rpm, uint32_t ms_desde_o_boot) {
  float fracao = (rpm - rpm_minimo) / (rpm_maximo - rpm_minimo);
  fracao = fracao < 0 ? 0 : fracao > 1 ? 1 : fracao;
  const float vibracao = vibracao_minima + (vibracao_maxima - vibracao_minima) * fracao;
  return Leitura{em_lsb(aceleracao + vibracao * ruido(ms_desde_o_boot, 0)),
                 em_lsb(vibracao * ruido(ms_desde_o_boot, 1)),
                 em_lsb(gravidade + vibracao * ruido(ms_desde_o_boot, 2))};
}

}  // namespace vguard::imu
