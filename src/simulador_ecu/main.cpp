// ECU simulada, no 2º ESP32, para os testes do OBD na bancada (proposta §4.7). Faz o papel da
// central do carro: responde aos pedidos do firmware com os mapas e os valores da tabela, de 5 a
// 30 ms depois, e manda um quadro de fundo a cada 20 ms. Por enquanto, só o cenário normal (#6);
// os outros, escolhidos por uma letra no monitor serial, vêm no #7.
//
// Os IDs e as respostas são escritos aqui, à parte do núcleo do firmware: um ID errado no
// firmware não pode se repetir no simulador e passar na bancada.
//
// Na USB do simulador sai texto livre, para o monitor serial (pio device monitor): ele não fala o
// protocolo da Pi.
#include <Arduino.h>
#include <SPI.h>
#include <esp_random.h>
#include <mcp_can.h>

#include <cstring>

#include "../firmware/config.h"

static_assert(__cplusplus >= 201703L, "o simulador usa C++17 (proposta §4.8)");

namespace {

using namespace vguard;

// Pedidos do firmware (proposta §4.7): o primeiro mapa a todos, o resto à central.
constexpr uint16_t id_todos = 0x7DF;
constexpr uint16_t id_central = 0x7E0;
// As respostas saem sempre em 0x7E8.
constexpr uint16_t id_resposta = 0x7E8;
// O tráfego de fundo: outro ID, a cada 20 ms. Sem ele, o firmware nunca começa a perguntar.
constexpr uint16_t id_fundo = 0x3A0;
constexpr uint32_t periodo_fundo_ms = 20;
constexpr uint8_t enchimento = 0x55;
// O tempo de resposta, sorteado a cada pedido.
constexpr uint32_t resposta_minima_ms = 5;
constexpr uint32_t resposta_maxima_ms = 30;
constexpr uint32_t periodo_resumo_ms = 5000;

// Uma resposta: <n> 41 <PID> <dados>, com o enchimento até o DLC 8. O n vale 3 para PID de 1
// byte, 4 para PID de 2 bytes e 6 para mapa.
struct Resposta {
  uint8_t pid;
  uint8_t n;
  uint8_t dados[4];
};

// Os mapas e os valores da proposta §4.7. Para simular outro carro, basta mudar esta tabela.
constexpr Resposta respostas[] = {
    {0x00, 6, {0x18, 0x3A, 0x80, 0x01}},  // 04, 05, 0B, 0C, 0D, 0F e 11; há outro mapa
    {0x20, 6, {0x00, 0x00, 0x00, 0x01}},  // nada de 21 a 3F; há outro mapa
    {0x40, 6, {0x40, 0x00, 0x00, 0x00}},  // 42
    {0x04, 3, {0x85}},                    // load 52,2 %
    {0x05, 3, {0x57}},                    // coolant 47 °C
    {0x0B, 3, {0x21}},                    // map 33 kPa
    {0x0C, 4, {0x0D, 0xB0}},              // rpm 876
    {0x0D, 3, {0x3C}},                    // speed 60 km/h
    {0x0F, 3, {0x46}},                    // iat 30 °C
    {0x11, 3, {0x26}},                    // throttle 14,9 %
    {0x42, 4, {0x35, 0xE8}},              // voltage 13,8 V
};

MCP_CAN controlador(&SPI, pinos::can_cs);

// A resposta do pedido em andamento. O firmware faz um pedido por vez.
const Resposta *pendente = nullptr;
uint32_t responder_em = 0;

uint32_t ultimo_fundo = 0;
uint32_t ultimo_resumo = 0;
uint8_t contador_fundo = 0;
uint32_t pedidos = 0;
uint32_t respondidos = 0;
uint32_t desconhecidos = 0;

// Máscara ou filtro de ID de 11 bits, no formato do mcp_can 1.5.1.
constexpr uint32_t filtro(uint16_t id) { return uint32_t(id & 0x7FF) << 16; }

// 500 kbit/s, cristal de 8 MHz, só os pedidos (0x7DF e 0x7E0) e envio único: enquanto o
// firmware só escuta, ninguém confirma (ACK) os quadros, e sem o envio único o MCP2515 os
// repetiria sem parar.
bool iniciar() {
  if (controlador.begin(MCP_STDEXT, CAN_500KBPS, MCP_8MHZ) != CAN_OK) {
    return false;
  }
  for (uint8_t mascara = 0; mascara < 2; ++mascara) {
    if (controlador.init_Mask(mascara, 0, filtro(0x7FF)) != CAN_OK) {
      return false;
    }
  }
  const uint16_t filtros[6] = {id_todos, id_central, id_todos, id_central, id_todos, id_central};
  for (uint8_t i = 0; i < 6; ++i) {
    if (controlador.init_Filt(i, 0, filtro(filtros[i])) != CAN_OK) {
      return false;
    }
  }
  return controlador.enOneShotTX() == CAN_OK && controlador.setMode(MCP_NORMAL) == CAN_OK;
}

void enviar(uint16_t id, uint8_t (&dados)[8]) { controlador.sendMsgBuf(id, 0, 8, dados); }

void mandar_fundo() {
  uint8_t dados[8] = {contador_fundo++, 0, 0, 0, 0, 0, 0, 0};
  enviar(id_fundo, dados);
}

void mandar_resposta(const Resposta &resposta) {
  uint8_t dados[8];
  std::memset(dados, enchimento, sizeof(dados));
  dados[0] = resposta.n;
  dados[1] = 0x41;
  dados[2] = resposta.pid;
  std::memcpy(dados + 3, resposta.dados, resposta.n - 2);
  enviar(id_resposta, dados);
  ++respondidos;
}

const Resposta *procurar(uint8_t pid) {
  for (const Resposta &resposta : respostas) {
    if (resposta.pid == pid) {
      return &resposta;
    }
  }
  return nullptr;
}

// Pedido do serviço 01 do firmware: 02 01 <PID>.
void receber(uint32_t agora) {
  while (controlador.checkReceive() == CAN_MSGAVAIL) {
    INT32U id = 0;
    INT8U dlc = 0;
    // O readMsgBuf() copia tantos bytes quanto o DLC diz, que pode ir até 15.
    INT8U dados[16] = {};
    if (controlador.readMsgBuf(&id, &dlc, dados) != CAN_OK) {
      return;
    }
    if ((id != id_todos && id != id_central) || dlc < 3 || dados[0] != 2 || dados[1] != 1) {
      continue;
    }
    ++pedidos;
    pendente = procurar(dados[2]);
    if (pendente == nullptr) {
      ++desconhecidos;
      Serial.printf("pedido do PID %02X, que o simulador não tem\n", dados[2]);
      continue;
    }
    const uint32_t faixa = resposta_maxima_ms - resposta_minima_ms + 1;
    responder_em = agora + resposta_minima_ms + esp_random() % faixa;
  }
}

}  // namespace

void setup() {
  Serial.begin(config::serial_baud);
  SPI.begin(pinos::can_sck, pinos::can_miso, pinos::can_mosi, pinos::can_cs);
  Serial.printf("\nSimulador da ECU do V-GUARD %s: 500 kbit/s, cristal de 8 MHz\n",
                versao_firmware);
  while (!iniciar()) {
    Serial.println("O MCP2515 não respondeu. Confira a alimentação e o SPI; tento de novo em 1 s.");
    delay(1000);
  }
  Serial.println("Cenário: normal. Respondendo em 0x7E8, fundo em 0x3A0 a cada 20 ms.");
  ultimo_fundo = ultimo_resumo = millis();
}

void loop() {
  const uint32_t agora = millis();
  receber(agora);
  if (pendente != nullptr && int32_t(agora - responder_em) >= 0) {
    mandar_resposta(*pendente);
    pendente = nullptr;
  }
  if (agora - ultimo_fundo >= periodo_fundo_ms) {
    ultimo_fundo += periodo_fundo_ms;
    mandar_fundo();
  }
  if (agora - ultimo_resumo >= periodo_resumo_ms) {
    ultimo_resumo = agora;
    Serial.printf("pedidos %lu, respondidos %lu, PIDs desconhecidos %lu, erros de envio %u/%u\n",
                  static_cast<unsigned long>(pedidos), static_cast<unsigned long>(respondidos),
                  static_cast<unsigned long>(desconhecidos), controlador.errorCountTX(),
                  controlador.errorCountRX());
  }
  delay(1);
}
