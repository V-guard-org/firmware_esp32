// O acelerômetro do ambiente esp32-sim (proposta §4.7, #19). No boot, tenta o ADXL345: se ele
// responder, é o sensor de verdade até o próximo boot, como no esp32; se não, o simulado do
// núcleo, também até o próximo boot. Assim, com o sensor ligado, a mesma placa testa o ADXL345
// real com o OBD simulado, sem regravar.
//
// O simulado conta o tempo desde o boot, como a central simulada do OBD, e as duas viagens andam
// juntas: o carro acelera no OBD e a leitura mostra a aceleração para a frente.
#include <esp_timer.h>

#include "acelerometro.h"
#include "adxl345.h"
#include "central_simulada.h"
#include "imu_simulado.h"

namespace vguard::acelerometro {

namespace {

// Decidida no primeiro iniciar(), o do boot.
enum class Fonte : uint8_t { a_decidir, sensor, simulado };
Fonte fonte = Fonte::a_decidir;

uint32_t agora_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

}  // namespace

void instalar() { adxl345::instalar(); }

bool iniciar() {
  if (fonte == Fonte::simulado) {
    return true;
  }
  const bool respondeu = adxl345::iniciar();
  if (fonte == Fonte::a_decidir) {
    fonte = respondeu ? Fonte::sensor : Fonte::simulado;
  }
  return respondeu || fonte == Fonte::simulado;
}

bool ler(imu::Leitura &leitura) {
  if (fonte != Fonte::simulado) {
    return adxl345::ler(leitura);
  }
  const uint32_t agora = agora_ms();
  const obd::Viagem viagem = obd::viagem(agora);
  leitura = imu::leitura_simulada(viagem.aceleracao, viagem.rpm, agora);
  return true;
}

bool simulado() { return fonte == Fonte::simulado; }

}  // namespace vguard::acelerometro
