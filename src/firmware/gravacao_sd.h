// O log no microSD (proposta §4.5 e §4.6): o cartão no 2º SPI (HSPI: SCK 27, MISO 32, MOSI 33,
// CS 26), a fila na RAM e a tarefa da gravação no núcleo 0. O que vai para o cartão, e em que
// arquivo, quem decide é o Registro do núcleo (lib/nucleo/arquivo_sd).
//
// Separado do MCP2515 porque o cartão às vezes para de 10 a 200 ms numa gravação, e o MCP2515
// guarda só 2 quadros. A aquisição nunca espera o cartão: só põe as linhas na fila.
#pragma once

#include <cstddef>
#include <cstdint>

#include "protocolo.h"

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
