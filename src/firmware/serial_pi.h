// A serial com a Pi pela USB (proposta §5): 115200 baud, 8N1, sem controle de fluxo.
//
// Saída: quem emite uma linha não espera a serial. A linha vai para um buffer na RAM, e uma
// tarefa própria a passa para a UART. Se ela não couber no buffer, é descartada e somada em
// descartadas (proposta §5.5).
//
// Entrada: outra tarefa lê as linhas da Pi (proposta §5.4). O $H acerta o relógio e põe o
// ESP32 em ATIVO; o $C,obd_hz muda a taxa do OBD. O $H ou o $C inválido gera um $L AVISO.
//
// O t de toda linha emitida vem desse relógio: o horário Unix em ms, ou 0 em ESPERA. As
// funções podem ser chamadas de qualquer tarefa.
#pragma once

#include <cstdint>

#include "protocolo.h"

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

// Recebeu algum $H: o ESP32 está em ATIVO.
bool ativo();

}  // namespace vguard::serial_pi
