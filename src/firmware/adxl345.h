// O ADXL345 no I²C (proposta §4.4): SDA GPIO21, SCL GPIO22, 400 kHz, endereço 0x53.
//
// Usa o driver I²C do ESP-IDF, instalado uma vez no setup(), e não o Wire: o Wire.end() e o
// Wire.begin() devolvem e pedem memória a cada chamada, e destravar o barramento em ATIVO
// faria isso a cada erro de leitura (CODING_STANDARDS.md).
//
// Só a tarefa de aquisição usa, depois do setup().
#pragma once

#include "imu.h"

namespace vguard::adxl345 {

// Instala o driver I²C. Uma vez, no setup().
void instalar();

// Destrava o barramento e inicia o sensor, no boot e a cada erro de leitura:
// 1. destrava: tira os pinos do controlador I²C, pulsa o SCL até 9 vezes até o SDA soltar, gera
//    um STOP à mão e devolve os pinos. Se o ESP32 reiniciou no meio de uma leitura, na partida
//    do motor, o sensor pode estar prendendo o SDA;
// 2. confere que o DEVID (0x00) devolve 0xE5;
// 3. DATA_FORMAT (0x31) = 0x0B: ±16 g com resolução completa, ~3,9 mg/LSB;
// 4. BW_RATE (0x2C) = 0x0A: 100 Hz;
// 5. POWER_CTL (0x2D) = 0x08, sempre por último: começa a medir.
// Devolve false se o sensor não respondeu ou o DEVID veio errado.
bool iniciar();

// Lê os três eixos de uma vez (DATAX0 a DATAZ1), para não misturar duas amostras. Devolve
// false num erro de I²C.
bool ler(imu::Leitura &leitura);

}  // namespace vguard::adxl345
