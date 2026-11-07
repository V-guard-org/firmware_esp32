// Configuração do firmware: pinos (proposta §4.6), endereço do ADXL345 e versão (§4.8).
// O simulador também inclui este arquivo, para usar os mesmos pinos do MCP2515.
#pragma once

#include <cstdint>

namespace vguard {

// <versão>-<commit>[-dirty], montada pelo firmware_version.py.
constexpr const char *versao_firmware = VGUARD_VERSAO_FIRMWARE;

namespace pinos {

// MCP2515, no 1º SPI (VSPI), onde o guia de ligações v3 manda.
constexpr uint8_t can_sck = 18;
constexpr uint8_t can_miso = 19;
constexpr uint8_t can_mosi = 23;
constexpr uint8_t can_cs = 25;

// microSD, no 2º SPI (HSPI), pela matriz de GPIO. Nenhum destes tem função no boot.
constexpr uint8_t sd_sck = 27;
constexpr uint8_t sd_miso = 32;
constexpr uint8_t sd_mosi = 33;
constexpr uint8_t sd_cs = 26;

// ADXL345, no I²C.
constexpr uint8_t imu_sda = 21;
constexpr uint8_t imu_scl = 22;

}  // namespace pinos

// Endereço do ADXL345 no I²C.
constexpr uint8_t endereco_imu = 0x53;

}  // namespace vguard
