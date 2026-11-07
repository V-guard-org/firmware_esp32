// Configuração do firmware: pinos (proposta §4.6), endereço do ADXL345 e versão (§4.8).
// O simulador também inclui este arquivo, para usar os mesmos pinos do MCP2515.
#pragma once

#include <cstddef>
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

namespace config {

// Serial com a Pi pela USB (proposta §5): 115200 baud, 8N1, sem controle de fluxo.
constexpr unsigned long serial_baud = 115200;
// Buffer na RAM antes da UART: ~0,2 s da serial a toda velocidade, ou ~0,7 s da carga normal
// (~3 KB/s com o OBD a 12,5 pedidos/s, proposta §5.5).
constexpr size_t serial_tamanho_saida = 2048;
// A tarefa que passa o buffer para a UART: pilha em bytes e prioridade (acima do loop()).
constexpr uint32_t serial_pilha_escrita = 3072;
constexpr unsigned serial_prioridade_escrita = 2;
// A tarefa que lê as linhas da Pi ($H e $C): pilha, prioridade (a do loop()) e intervalo entre
// as leituras. Os 10 ms entram no máximo uma vez no erro da hora.
constexpr uint32_t serial_pilha_leitura = 3072;
constexpr unsigned serial_prioridade_leitura = 1;
constexpr uint32_t serial_periodo_leitura_ms = 10;

// Taxa do OBD ao ligar, em décimos de pedido por segundo: 12,5 (proposta §4.3).
constexpr uint16_t obd_hz_inicial_decimos = 125;

// Período do $S (proposta §5.3).
constexpr uint32_t periodo_status_ms = 1000;

}  // namespace config

}  // namespace vguard
