// O acelerômetro, visto pela tarefa de aquisição. Duas implementações, uma por ambiente:
// - acelerometro_adxl345.cpp (esp32): o ADXL345 no I²C (adxl345.h);
// - acelerometro_simulado.cpp (esp32-sim): o ADXL345, se ele responder no boot; senão, o
//   simulado do núcleo, que acompanha a viagem do OBD simulado (proposta §4.7, #19).
//
// Só a tarefa de aquisição usa, depois do setup(), e o setup(), antes de criá-la.
#pragma once

#include "imu.h"

namespace vguard::acelerometro {

// Uma vez, no setup().
void instalar();

// No boot e a cada erro de leitura, como o adxl345::iniciar(). Devolve false se o sensor não
// respondeu.
bool iniciar();

// Lê os três eixos de uma vez. Devolve false num erro de leitura.
bool ler(imu::Leitura &leitura);

// As leituras são as do simulado (só no esp32-sim, sem o sensor no boot).
bool simulado();

}  // namespace vguard::acelerometro
