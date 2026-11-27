// Pinos do can_sd_adxl_soldado. Taxas e protocolo do firmware do colega.
#pragma once
#include <cstddef>
#include <cstdint>

namespace vguard {
constexpr const char *versao_firmware = "0.10.1-soldado-rtos";
namespace pinos {
constexpr uint8_t can_sck = 14, can_miso = 12, can_mosi = 13, can_cs = 15;
constexpr uint8_t sd_sck = 18, sd_miso = 19, sd_mosi = 23, sd_cs = 5;
constexpr uint8_t imu_sda = 21, imu_scl = 22;
}
constexpr uint8_t endereco_imu = 0x53;

namespace config {
// Sem um $H válido, inicia aquisição e log após 120 s desde o boot.
constexpr uint64_t espera_hora_ms = 120000;
// Núcleo 1: aquisição. Núcleo 0: SD, serial e status.
constexpr int aquisicao_nucleo = 1;
constexpr int saida_nucleo = 0;
constexpr int sd_nucleo = saida_nucleo;
constexpr uint32_t aquisicao_pilha = 4096;
constexpr unsigned aquisicao_prioridade = 5;
constexpr uint32_t aquisicao_periodo_ms = 10; // IMU 100 Hz; saída filtrada 50 Hz.

// 12,5 pedidos/s no TOTAL, distribuídos pelos PIDs suportados.
// A Pi pode alterar a taxa com $C,obd_hz,... (protocolo original).
constexpr uint16_t obd_hz_inicial_decimos = 125;
constexpr uint32_t obd_quadros_por_ciclo = 24;

constexpr uint32_t imu_i2c_hz = 400000;
constexpr uint32_t imu_i2c_espera_ms = 10;
constexpr uint32_t imu_intervalo_tentativas_ms = 1000;
constexpr uint32_t imu_espera_primeira_amostra_ms = 20;

constexpr unsigned long serial_baud = 115200;
constexpr size_t serial_tamanho_saida = 2048;
constexpr uint32_t serial_pilha_escrita = 3072;
constexpr unsigned serial_prioridade_escrita = 2;
constexpr uint32_t serial_pilha_leitura = 3072;
constexpr unsigned serial_prioridade_leitura = 1;
constexpr uint32_t serial_periodo_leitura_ms = 10;
constexpr uint32_t periodo_status_ms = 1000;
constexpr uint32_t status_pilha = 3072;
constexpr unsigned status_prioridade = 1;

constexpr size_t sd_tamanho_fila = 16384;
constexpr size_t sd_tamanho_bloco = 8192;
constexpr uint32_t sd_periodo_gravacao_ms = 1000;
constexpr uint32_t sd_periodo_livre_ms = 60000;
constexpr uint32_t sd_spi_hz = 4000000;
constexpr uint32_t sd_pilha = 6144;
constexpr unsigned sd_prioridade = 1;
}
}
