// O barramento CAN do OBD, visto pela tarefa de aquisição. Duas implementações, uma por ambiente:
// - can_mcp2515.cpp (esp32): o MCP2515 no 1º SPI (proposta §4.3 e §4.6), SCK 18, MISO 19,
//   MOSI 23, CS 25, cristal de 8 MHz, 500 kbit/s, IDs de 11 bits, em volta do mcp_can 1.5.1,
//   como no iniciarControlador() do voyage_painel.ino (repositório OBD-II);
// - can_simulado.cpp (esp32-sim): a central simulada do núcleo, sem CAN (proposta §4.7, #17).
//
// Só a tarefa de aquisição usa, depois do setup().
#pragma once

#include <cstdint>

#include "obd.h"

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
