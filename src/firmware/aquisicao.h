// A aquisição (proposta §4.6): a tarefa de prioridade alta que, em ATIVO, a cada 10 ms, lê o CAN
// e o ADXL345. Em ESPERA, ela não lê nenhum dos dois.
//
// O OBD (proposta §4.3): no primeiro ciclo em ATIVO, o MCP2515 é iniciado em só escuta; se não
// responder, obd FALHA até o próximo boot. Depois, os quadros recebidos (até 24 por ciclo) vão à
// máquina do OBD, que descobre os PIDs, pede os 8 de interesse e emite o $P e os $O.
//
// O sensor (proposta §4.4):
// - no boot, antes do $H, ele é iniciado e passa pela conferência de 1 g, com 10 leituras. É a
//   única leitura dele em ESPERA;
// - num erro de leitura, o barramento é destravado e o sensor iniciado de novo, na hora. Se
//   não voltar, fica fora: imu FALHA, nenhum $A e $L,…,ERRO,IMU sem resposta;
// - fora, ele é tentado de novo a cada 1 s, só em ATIVO. Quando volta: $L,…,AVISO,IMU voltou, e
//   o imu volta ao resultado da conferência do boot. Se o boot não a fez, ela é feita na volta.
#pragma once

#include "protocolo.h"

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
