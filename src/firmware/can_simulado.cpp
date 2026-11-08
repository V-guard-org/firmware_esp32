// O barramento CAN do ambiente esp32-sim (proposta §4.7, #17): no lugar do MCP2515, a central
// simulada do núcleo, que responde com a viagem em laço. Para a bancada que testa a integração
// sem o carro; o MCP2515 de verdade é testado no carro.
//
// A resposta de cada pedido chega na próxima leitura, no ciclo seguinte da aquisição (10 ms), e
// um quadro de fundo a cada 20 ms faz a descoberta começar, como o tráfego do carro.
#include <esp_timer.h>

#include "can.h"
#include "central_simulada.h"

namespace vguard::can {

namespace {

// O tráfego de fundo: outro ID, a cada 20 ms.
constexpr uint32_t id_fundo = 0x3A0;
constexpr uint32_t periodo_fundo_ms = 20;

bool tem_resposta = false;
obd::Quadro resposta{};
uint32_t ultimo_fundo = 0;
uint8_t contador_fundo = 0;

// Os ms desde o boot: a viagem e o aquecimento contam a partir do boot.
uint32_t agora_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

}  // namespace

void preparar() {}

bool iniciar_escuta() {
  tem_resposta = false;
  return true;
}

bool configurar_leitura() {
  tem_resposta = false;
  return true;
}

bool enviar(uint16_t id, const uint8_t (&dados)[8]) {
  tem_resposta = obd::responder(id, dados, agora_ms(), resposta);
  return true;
}

bool receber(obd::Quadro &quadro) {
  if (tem_resposta) {
    tem_resposta = false;
    quadro = resposta;
    return true;
  }
  const uint32_t agora = agora_ms();
  if (agora - ultimo_fundo < periodo_fundo_ms) {
    return false;
  }
  ultimo_fundo = agora;
  quadro = obd::Quadro{id_fundo, 8, {contador_fundo++, 0, 0, 0, 0, 0, 0, 0}};
  return true;
}

bool erro_no_barramento() { return false; }

}  // namespace vguard::can
