// ECU simulada, no 2º ESP32, para os testes do OBD na bancada (proposta §4.7). Faz o papel da
// central do carro: responde aos pedidos do firmware com os mapas e os valores da tabela, de 5 a
// 30 ms depois, e manda um quadro de fundo a cada 20 ms.
//
// Os cenários de falha (#7) são escolhidos por uma letra no monitor serial: n normal, s PID 0F sem
// resposta, a PID 0F com "aguarde", r PID 0F recusado, m central muda, d carro desligado e 2
// segunda central em 0x7E9.
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

// Os cenários de um PID só usam o 0F (iat), o do exemplo da proposta §5.6.
constexpr uint8_t pid_do_cenario = 0x0F;
// No "aguarde", a resposta de verdade sai 400 ms depois do pedido, antes dos 1200 ms do firmware.
constexpr uint32_t aguarde_resposta_ms = 400;
constexpr uint8_t codigo_aguarde = 0x78;
// A recusa: 0x31, "fora da faixa", o código de quem não tem o PID.
constexpr uint8_t codigo_recusa = 0x31;
// A segunda central responde em 0x7E9, junto com a primeira, com valores que não existem: se
// passassem pelo filtro, apareceriam no $O e no $P.
constexpr uint16_t id_segunda_central = 0x7E9;
constexpr uint8_t valor_da_segunda_central = 0xFF;

enum class Cenario : uint8_t { normal, sem_resposta, aguarde, recusa, muda, desligado, segunda };

struct DescricaoDoCenario {
  Cenario cenario;
  char letra;
  const char *texto;
};

constexpr DescricaoDoCenario cenarios[] = {
    {Cenario::normal, 'n', "normal"},
    {Cenario::sem_resposta, 's', "PID 0F sem resposta"},
    {Cenario::aguarde, 'a', "PID 0F com \"aguarde\" (resposta em 400 ms)"},
    {Cenario::recusa, 'r', "PID 0F recusado (03 7F 01 31)"},
    {Cenario::muda, 'm', "central muda (o fundo continua)"},
    {Cenario::desligado, 'd', "carro desligado (tudo em silêncio, sem ACK)"},
    {Cenario::segunda, '2', "segunda central em 0x7E9 (valores FF)"},
};

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

Cenario cenario = Cenario::normal;

// Os quadros que vão sair, e quando. O firmware faz um pedido por vez, mas um pedido pode gerar
// dois quadros: o "aguarde" e a resposta, ou as respostas das duas centrais.
struct Envio {
  bool marcado;
  uint32_t em;
  uint16_t id;
  uint8_t dados[8];
};
Envio envios[2] = {};

uint32_t ultimo_fundo = 0;
uint32_t ultimo_resumo = 0;
uint8_t contador_fundo = 0;
uint32_t pedidos = 0;
uint32_t respondidos = 0;
uint32_t negativas = 0;
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

void desmarcar_envios() {
  for (Envio &envio : envios) {
    envio.marcado = false;
  }
}

// Marca um quadro de 8 bytes, com o enchimento depois dos `tamanho` primeiros.
void marcar(uint32_t em, uint16_t id, const uint8_t *dados, size_t tamanho) {
  for (Envio &envio : envios) {
    if (!envio.marcado) {
      envio.marcado = true;
      envio.em = em;
      envio.id = id;
      std::memset(envio.dados, enchimento, sizeof(envio.dados));
      std::memcpy(envio.dados, dados, tamanho);
      return;
    }
  }
}

// <n> 41 <PID> <dados>. A segunda central põe FF em todos os bytes de dados.
void marcar_resposta(uint32_t em, uint16_t id, const Resposta &resposta, bool segunda) {
  uint8_t dados[7] = {resposta.n, 0x41, resposta.pid};
  std::memcpy(dados + 3, resposta.dados, resposta.n - 2);
  if (segunda) {
    std::memset(dados + 3, valor_da_segunda_central, resposta.n - 2);
  }
  marcar(em, id, dados, resposta.n + 1);
}

// 03 7F 01 <código>: a recusa ou o "aguarde".
void marcar_negativa(uint32_t em, uint8_t codigo) {
  const uint8_t dados[4] = {3, 0x7F, 1, codigo};
  marcar(em, id_resposta, dados, sizeof(dados));
}

void mandar_envios(uint32_t agora) {
  for (Envio &envio : envios) {
    if (envio.marcado && int32_t(agora - envio.em) >= 0) {
      envio.marcado = false;
      enviar(envio.id, envio.dados);
      if (envio.id != id_resposta) {
        continue;
      }
      if (envio.dados[1] == 0x41) {
        ++respondidos;
      } else {
        ++negativas;
      }
    }
  }
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
    // Um pedido novo desmarca o que ainda ia sair do anterior.
    desmarcar_envios();
    if (cenario == Cenario::muda || cenario == Cenario::desligado) {
      continue;
    }
    const uint8_t pid = dados[2];
    const Resposta *resposta = procurar(pid);
    if (resposta == nullptr) {
      ++desconhecidos;
      Serial.printf("pedido do PID %02X, que o simulador não tem\n", pid);
      continue;
    }
    const uint32_t faixa = resposta_maxima_ms - resposta_minima_ms + 1;
    const uint32_t em = agora + resposta_minima_ms + esp_random() % faixa;
    const bool do_cenario = pid == pid_do_cenario;
    if (cenario == Cenario::sem_resposta && do_cenario) {
      continue;
    }
    if (cenario == Cenario::recusa && do_cenario) {
      marcar_negativa(em, codigo_recusa);
    } else if (cenario == Cenario::aguarde && do_cenario) {
      marcar_negativa(em, codigo_aguarde);
      marcar_resposta(agora + aguarde_resposta_ms, id_resposta, *resposta, false);
    } else if (cenario == Cenario::segunda) {
      // A segunda central responde antes, para o firmware ver o quadro dela primeiro.
      marcar_resposta(em, id_segunda_central, *resposta, true);
      marcar_resposta(em + 1, id_resposta, *resposta, false);
    } else {
      marcar_resposta(em, id_resposta, *resposta, false);
    }
  }
}

const DescricaoDoCenario &descricao(Cenario qual) {
  for (const DescricaoDoCenario &d : cenarios) {
    if (d.cenario == qual) {
      return d;
    }
  }
  return cenarios[0];
}

void mostrar_cenarios() {
  Serial.println("Cenários (digite a letra):");
  for (const DescricaoDoCenario &d : cenarios) {
    Serial.printf("  %c  %s\n", d.letra, d.texto);
  }
}

// Troca de cenário. No carro desligado, o MCP2515 passa a só escutar: não manda nada nem confirma
// (ACK) os pedidos, como uma central desligada. A troca desmarca o que ia sair.
void trocar_cenario(Cenario novo) {
  const bool escutar = novo == Cenario::desligado;
  if (escutar != (cenario == Cenario::desligado) &&
      controlador.setMode(escutar ? MCP_LISTENONLY : MCP_NORMAL) != CAN_OK) {
    Serial.println("O MCP2515 não trocou de modo; o cenário continua o mesmo.");
    return;
  }
  cenario = novo;
  desmarcar_envios();
  Serial.printf("Cenário: %s\n", descricao(cenario).texto);
}

// Uma letra do monitor serial escolhe o cenário. O Enter e os espaços são ignorados.
void ler_monitor() {
  while (Serial.available() > 0) {
    const int letra = Serial.read();
    if (letra == '\r' || letra == '\n' || letra == ' ') {
      continue;
    }
    bool achou = false;
    for (const DescricaoDoCenario &d : cenarios) {
      if (d.letra == letra) {
        trocar_cenario(d.cenario);
        achou = true;
      }
    }
    if (!achou) {
      mostrar_cenarios();
    }
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
  Serial.println("Respondendo em 0x7E8, fundo em 0x3A0 a cada 20 ms.");
  mostrar_cenarios();
  Serial.printf("Cenário: %s\n", descricao(cenario).texto);
  ultimo_fundo = ultimo_resumo = millis();
}

void loop() {
  const uint32_t agora = millis();
  ler_monitor();
  receber(agora);
  mandar_envios(agora);
  if (agora - ultimo_fundo >= periodo_fundo_ms) {
    ultimo_fundo += periodo_fundo_ms;
    if (cenario != Cenario::desligado) {
      mandar_fundo();
    }
  }
  if (agora - ultimo_resumo >= periodo_resumo_ms) {
    ultimo_resumo = agora;
    Serial.printf(
        "[%c] pedidos %lu, respondidos %lu, negativas %lu, PIDs desconhecidos %lu, erros de "
        "envio %u/%u\n",
        descricao(cenario).letra, static_cast<unsigned long>(pedidos),
        static_cast<unsigned long>(respondidos), static_cast<unsigned long>(negativas),
        static_cast<unsigned long>(desconhecidos), controlador.errorCountTX(),
        controlador.errorCountRX());
  }
  delay(1);
}
