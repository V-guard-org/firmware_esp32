// A serial com a Pi pela USB (proposta §5): 115200 baud, 8N1, sem controle de fluxo.
//
// Quem emite uma linha não espera a serial: a linha vai para um buffer na RAM, e uma tarefa
// própria a passa para a UART. Se ela não couber no buffer, é descartada e somada em
// descartadas (proposta §5.5). As funções podem ser chamadas de qualquer tarefa.
#pragma once

#include <cstdint>

#include "protocolo.h"

namespace vguard::serial_pi {

// Abre a serial e cria o buffer, a trava e a tarefa de escrita. Uma vez, no setup().
void iniciar();

// $B, com a versão do firmware.
void emitir_boot(uint64_t t, protocolo::MotivoReset motivo);
// $S. As descartadas vêm da própria saída.
void emitir_status(uint64_t t, const protocolo::Status &status);

}  // namespace vguard::serial_pi
