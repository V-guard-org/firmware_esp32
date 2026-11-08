// O acelerômetro do ambiente esp32 (o do carro): sempre o ADXL345 de verdade.
#include "acelerometro.h"
#include "adxl345.h"

namespace vguard::acelerometro {

void instalar() { adxl345::instalar(); }

bool iniciar() { return adxl345::iniciar(); }

bool ler(imu::Leitura &leitura) { return adxl345::ler(leitura); }

bool simulado() { return false; }

}  // namespace vguard::acelerometro
