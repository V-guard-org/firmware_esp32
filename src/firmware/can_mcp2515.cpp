#include "can_mcp2515.h"

#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>

#include <cstring>

#include "config.h"

namespace vguard::can_mcp2515 {

namespace {

// O global SPI do Arduino-ESP32 é o VSPI, o 1º SPI.
MCP_CAN controlador(&SPI, pinos::can_cs);

// Inicia o controlador (o begin() o deixa em loopback), com ou sem os filtros, e liga o envio
// único. Copiada do iniciarControlador() do voyage_painel.ino.
bool iniciar(bool filtrar) {
  if (controlador.begin(MCP_STDEXT, CAN_500KBPS, MCP_8MHZ) != CAN_OK) {
    return false;
  }
  if (filtrar) {
    for (uint8_t mascara = 0; mascara < 2; ++mascara) {
      if (controlador.init_Mask(mascara, 0, obd::filtro_padrao(0x7FF)) != CAN_OK) {
        return false;
      }
    }
    for (uint8_t filtro = 0; filtro < 6; ++filtro) {
      if (controlador.init_Filt(filtro, 0, obd::filtro_padrao(obd::id_resposta)) != CAN_OK) {
        return false;
      }
    }
  }
  if (controlador.enOneShotTX() != CAN_OK) {
    return false;
  }
  return controlador.setMode(filtrar ? MCP_NORMAL : MCP_LISTENONLY) == CAN_OK;
}

}  // namespace

void preparar() { SPI.begin(pinos::can_sck, pinos::can_miso, pinos::can_mosi, pinos::can_cs); }

bool iniciar_escuta() { return iniciar(false); }

bool configurar_leitura() { return iniciar(true); }

bool enviar(uint16_t id, const uint8_t (&dados)[8]) {
  uint8_t copia[8];
  std::memcpy(copia, dados, sizeof(copia));
  return controlador.sendMsgBuf(id, 0, sizeof(copia), copia) == CAN_OK;
}

bool receber(obd::Quadro &quadro) {
  if (controlador.checkReceive() != CAN_MSGAVAIL) {
    return false;
  }
  // O readMsgBuf() copia tantos bytes quanto o DLC diz, e o DLC de um quadro pode ir até 15:
  // a cópia vem para um buffer que aguenta isso, e só os 8 primeiros entram no quadro.
  INT32U id = 0;
  INT8U dlc = 0;
  INT8U dados[16] = {};
  if (controlador.readMsgBuf(&id, &dlc, dados) != CAN_OK) {
    return false;
  }
  quadro.id = id;
  quadro.dlc = dlc;
  std::memcpy(quadro.dados, dados, sizeof(quadro.dados));
  return true;
}

}  // namespace vguard::can_mcp2515
