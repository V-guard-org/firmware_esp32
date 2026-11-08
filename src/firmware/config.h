// Configuração do firmware: pinos (proposta §4.6), endereço do ADXL345, versão (§4.8), taxas,
// tempos e tamanhos.
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
// Quadros do CAN lidos por ciclo de 10 ms, no máximo, como no voyage_painel.ino: em só escuta, no
// carro, chegam centenas por segundo, e a leitura não pode tomar o ciclo inteiro.
constexpr uint32_t obd_quadros_por_ciclo = 24;

// Período do $S (proposta §5.3).
constexpr uint32_t periodo_status_ms = 1000;

// I²C do ADXL345 (proposta §4.4): 400 kHz. Cada leitura ou escrita espera no máximo este tempo;
// a leitura dos 3 eixos leva ~0,3 ms.
constexpr uint32_t imu_i2c_hz = 400000;
constexpr uint32_t imu_i2c_espera_ms = 10;

// A tarefa de aquisição (proposta §4.6): prioridade alta, acima da serial, no núcleo 1, o do
// loop(). A gravação do SD vai para o núcleo 0.
constexpr uint32_t aquisicao_pilha = 4096;
constexpr unsigned aquisicao_prioridade = 5;
constexpr int aquisicao_nucleo = 1;
// Uma leitura do ADXL345 a cada 10 ms (100 Hz), pelo relógio do ESP32. O $A sai em uma de cada
// duas.
constexpr uint32_t aquisicao_periodo_ms = 10;
// Com o sensor fora, uma tentativa de falar com ele a cada 1 s, só em ATIVO (proposta §4.4).
constexpr uint32_t imu_intervalo_tentativas_ms = 1000;
// Depois do POWER_CTL, a primeira amostra leva 1,1 ms mais um período de 10 ms. Antes disso, os
// registradores ainda têm zero, ou a amostra de antes do reinício.
constexpr uint32_t imu_espera_primeira_amostra_ms = 20;

// microSD (proposta §4.5). A fila na RAM guarda ~6 s de linhas (~2,6 KB/s com o OBD a 12,5
// pedidos/s): folga para as paradas de 10 a 200 ms do cartão numa gravação.
constexpr size_t sd_tamanho_fila = 16384;
// A cada 1 s, a tarefa tira a fila, grava e confirma no cartão. É o valor inicial; a equipe
// ajusta nos testes.
constexpr uint32_t sd_periodo_gravacao_ms = 1000;
// O que a tarefa junta na RAM antes de escrever no cartão: ~3 s de linhas.
constexpr size_t sd_tamanho_bloco = 8192;
// O sd_livre é medido de novo a cada 60 s.
constexpr uint32_t sd_periodo_livre_ms = 60000;
// O SPI do cartão: 4 MHz, como no sketch do Nicolas.
constexpr uint32_t sd_spi_hz = 4000000;
// A tarefa da gravação: pilha (o FATFS e o snprintf), prioridade baixa e o núcleo 0, longe da
// aquisição (proposta §4.6).
constexpr uint32_t sd_pilha = 6144;
constexpr unsigned sd_prioridade = 1;
constexpr int sd_nucleo = 0;

}  // namespace config

}  // namespace vguard
