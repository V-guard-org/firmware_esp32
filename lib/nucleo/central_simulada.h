// A central do carro simulada (proposta §4.7, #17): faz o papel da central do Voyage para a
// máquina do OBD, sem CAN, no ambiente esp32-sim. Responde aos mapas e aos 8 PIDs de interesse
// com os valores de uma viagem em laço de 300 s. Só memória de tamanho fixo.
//
// A bancada sem os dois MCP2515 testa a integração (OBD e acelerômetro no ESP32, microSD e Pi);
// o MCP2515 de verdade é testado no carro.
#pragma once

#include <cstdint>

#include "obd.h"

namespace vguard::obd {

// Os mapas do Voyage (proposta §4.7): 04, 05, 0B, 0C, 0D, 0F e 11 no mapa 00, nada de 21 a 3F
// no 20, e o 42 no 40.
constexpr uint32_t mapas_simulados[3] = {0x183A8001UL, 0x00000001UL, 0x40000000UL};

// A viagem dura 300 s e recomeça.
constexpr uint32_t duracao_da_viagem_ms = 300000;

// Os valores da viagem num instante, nas unidades do $O, e a aceleração longitudinal, para o
// acelerômetro simulado (#19).
struct Viagem {
  float speed;     // km/h
  float rpm;       // rpm
  float throttle;  // %
  float load;      // %
  float map;       // kPa
  float coolant;   // °C
  float iat;       // °C
  float voltage;   // V
  // A inclinação da velocidade no trecho, em m/s²: +1,11 acelerando de 0 a 80 km/h em 20 s,
  // −1,11 freando, e 0 no resto.
  float aceleracao;
};

// Os valores em ms desde o boot. A viagem é interpolada entre os pontos do perfil (#17); o
// coolant sobe de 47 °C no boot até 90 °C em 10 min.
Viagem viagem(uint32_t ms_desde_o_boot);

// Responde ao pedido da máquina (o ID e os 8 bytes, como a Saida::enviar recebe), em 0x7E8 com
// DLC 8 e enchimento 0x55. Devolve false se a central não responde: pedido que não é do serviço
// 01, para outro ID ou de um PID que ela não tem.
bool responder(uint16_t id, const uint8_t (&pedido)[8], uint32_t ms_desde_o_boot,
               Quadro &resposta);

}  // namespace vguard::obd
