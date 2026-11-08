// O ADXL345 simulado do ambiente esp32-sim (proposta §4.7, #19): a leitura crua que o sensor
// daria no carro da viagem simulada. Só memória de tamanho fixo.
//
// A simulação entra nas leituras cruas: o resto do acelerômetro (conferência de 1 g, filtro de
// 5 Hz, uma amostra de cada duas) roda como com o sensor de verdade.
#pragma once

#include <cstdint>

#include "imu.h"

namespace vguard::imu {

// A vibração vai de ±0,05 m/s² em marcha lenta (800 rpm) a ±0,3 m/s² a 3000 rpm, em cada eixo.
constexpr float vibracao_minima = 0.05f;
constexpr float vibracao_maxima = 0.3f;

// A leitura crua, em LSB de ~3,9 mg, com o x para a frente e o z para cima: x é a aceleração
// longitudinal (m/s²), y é 0 e z é 1 g, mais a vibração. A vibração é a mesma para o mesmo
// instante (ms desde o boot), para os testes.
Leitura leitura_simulada(float aceleracao, float rpm, uint32_t ms_desde_o_boot);

}  // namespace vguard::imu
