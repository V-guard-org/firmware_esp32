#pragma once
#include <cstddef>
#include <cstdint>
#include "src/nucleo/protocolo.h"
#include "src/nucleo/obd.h"
#include "src/nucleo/imu.h"

namespace vguard::can {

// No setup(): liga o 1º SPI nos pinos do MCP2515, sem falar com ele. Em ESPERA, o firmware não
// toca no MCP2515.
void preparar();

// Ao entrar em ATIVO, e de novo quando o OBD para: inicia o MCP2515 em só escuta, sem filtros e
// com envio único, o que zera os contadores de erro. Em só escuta, ele não transmite nem confirma
// (ACK) os quadros. Devolve false se ele não respondeu.
bool iniciar_escuta();

// Antes da descoberta: inicia de novo, com as máscaras e os 6 filtros aceitando só 0x7E8, e passa
// ao modo normal. Devolve false se ele não respondeu.
bool configurar_leitura();

// Manda um quadro de 11 bits com DLC 8, com envio único. Espera no máximo ~5 ms por um buffer de
// envio e pela transmissão. Devolve false se o quadro não saiu.
bool enviar(uint16_t id, const uint8_t (&dados)[8]);

// Lê um quadro recebido, se houver.
bool receber(obd::Quadro &quadro);

// O MCP2515 acusa erro passivo (na transmissão ou na recepção) ou bus-off, pelo registrador EFLG,
// como o voyage_painel.ino (getError() & 0x38).
bool erro_no_barramento();

}  // namespace vguard::can

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

namespace vguard::aquisicao {

// No setup(), antes do $B: liga o SPI do MCP2515 (sem falar com ele), instala o I²C, inicia o
// sensor e faz a conferência de 1 g.
void preparar();

// Cria a tarefa. No setup(), depois do $B e do primeiro $S.
void iniciar();

// O imu do $S: FALHA com o sensor fora ou com a conferência de 1 g fora da faixa. Na conferência
// fora da faixa, o $A continua, e a Pi decide. Pode ser chamada de qualquer tarefa.
protocolo::EstadoImu estado_imu();

// O obd do $S: PARADO em ESPERA, FALHA sem o MCP2515, ou o estado da máquina (ESCUTANDO,
// DESCOBRINDO ou LENDO). Pode ser chamada de qualquer tarefa.
protocolo::EstadoObd estado_obd();

}  // namespace vguard::aquisicao

namespace vguard::serial_pi {

// Abre a serial e cria o buffer, a trava e as tarefas de escrita e de leitura. Uma vez, no
// setup().
void iniciar();

// $B, com a versão do firmware.
void emitir_boot(protocolo::MotivoReset motivo);
// $S. O estado e o obd_hz vêm do $H e do $C recebidos, e as descartadas, da própria saída; os
// outros campos, de quem chama.
void emitir_status(protocolo::Status status);
// $L.
void emitir_log(protocolo::Nivel nivel, const char *texto);
// $A, com cada eixo em centésimos de m/s².
void emitir_acelerometro(int32_t ax, int32_t ay, int32_t az);
// $O, com o valor multiplicado por 10^casas.
void emitir_obd(const char *campo, int64_t valor_escalado, uint8_t casas);
// $P, com os três mapas de PIDs suportados.
void emitir_pids(uint32_t mapa00, uint32_t mapa20, uint32_t mapa40);

// A taxa do OBD em uso, pelo último $C válido: em décimos de pedido por segundo (125 é 12.5).
uint16_t obd_hz_decimos();

// Recebeu algum $H válido ou entrou em fallback após 120 s.
bool ativo();

}  // namespace vguard::serial_pi

namespace vguard::gravacao_sd {

// No setup(), antes do $B: monta o cartão e reserva a fila. Sem cartão, sd AUSENTE até o
// próximo boot.
void preparar();

// No fim do setup(): cria a tarefa da gravação, se o cartão montou. A cada 1 s, ela passa a fila
// ao Registro, grava o que juntou e confirma no cartão.
void iniciar();

// Põe a linha, com a moldura, na fila do SD, sem esperar. A linha que não cabe fica de fora do
// SD e é contada; quando a gravação volta, sai um $L "SD perdeu N linhas".
//
// Quem chama segura a trava da serial: assim a ordem da fila é a da emissão, e há um só
// escritor.
void enfileirar(const char *linha, size_t tamanho);

// O sd do $S: OK com o cartão montado (em ESPERA também), AUSENTE sem cartão no boot e FALHA
// depois de um erro de gravação, até o próximo boot.
protocolo::EstadoSd estado();

// O sd_livre do $S: espaço livre no cartão, em %. Medido na montagem e depois a cada 60 s.
uint8_t livre();

}  // namespace vguard::gravacao_sd
