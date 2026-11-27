#include "imu.h"

#include <cmath>

namespace vguard::imu {

namespace {

constexpr float ms2_por_lsb = g_por_lsb * gravidade;

float filtrar(float anterior, float novo) { return alfa * novo + (1.0f - alfa) * anterior; }

}  // namespace

Aceleracao em_ms2(Leitura crua, Aceleracao offset) {
  return Aceleracao{crua.x * ms2_por_lsb - offset.x, crua.y * ms2_por_lsb - offset.y,
                    crua.z * ms2_por_lsb - offset.z};
}

int32_t centesimos(float valor) { return static_cast<int32_t>(std::lround(valor * 100.0f)); }

bool conferir_1g(const Leitura *leituras, size_t quantidade, Aceleracao offset) {
  if (leituras == nullptr || quantidade == 0) {
    return false;
  }
  // A soma cabe com folga: 13 bits por leitura.
  int32_t soma_x = 0;
  int32_t soma_y = 0;
  int32_t soma_z = 0;
  for (size_t i = 0; i < quantidade; ++i) {
    soma_x += leituras[i].x;
    soma_y += leituras[i].y;
    soma_z += leituras[i].z;
  }
  const float n = static_cast<float>(quantidade);
  const float x = soma_x / n * ms2_por_lsb - offset.x;
  const float y = soma_y / n * ms2_por_lsb - offset.y;
  const float z = soma_z / n * ms2_por_lsb - offset.z;
  const float modulo_g = std::sqrt(x * x + y * y + z * z) / gravidade;
  return modulo_g >= conferencia_minimo_g && modulo_g <= conferencia_maximo_g;
}

bool Tratamento::receber(Leitura crua, Aceleracao &saida) {
  const Aceleracao nova = em_ms2(crua, offset_);
  if (!comecou_) {
    filtrada_ = nova;
    comecou_ = true;
    sai_a_proxima_ = true;
  } else {
    filtrada_ = Aceleracao{filtrar(filtrada_.x, nova.x), filtrar(filtrada_.y, nova.y),
                           filtrar(filtrada_.z, nova.z)};
  }
  const bool sai = sai_a_proxima_;
  sai_a_proxima_ = !sai_a_proxima_;
  if (sai) {
    saida = filtrada_;
  }
  return sai;
}

}  // namespace vguard::imu
