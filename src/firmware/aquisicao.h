// A aquisição (proposta §4.6): a tarefa de prioridade alta que lê o ADXL345 a 100 Hz e, em
// ATIVO, manda o $A a 50 Hz. Depois, ela recebe também o CAN.
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

// No setup(), antes do $B: instala o I²C, inicia o sensor e faz a conferência de 1 g.
void preparar();

// Cria a tarefa. No setup(), depois do $B e do primeiro $S.
void iniciar();

// O imu do $S: FALHA com o sensor fora ou com a conferência de 1 g fora da faixa. Na conferência
// fora da faixa, o $A continua, e a Pi decide. Pode ser chamada de qualquer tarefa.
protocolo::EstadoImu estado_imu();

}  // namespace vguard::aquisicao
