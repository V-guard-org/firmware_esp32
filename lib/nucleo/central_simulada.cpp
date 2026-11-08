#include "central_simulada.h"

#include <cmath>
#include <cstring>

namespace vguard::obd {

namespace {

// Um ponto do perfil: o instante no laço e os valores que variam com a viagem.
struct Ponto {
  uint32_t ms;
  float speed;
  float rpm;
  float throttle;
  float load;
  float map;
};

// A viagem da Issue #17: parado, acelera até 80 km/h, cruzeiro, freia e para.
constexpr Ponto perfil[] = {
    {0, 0, 800, 15, 20, 30},          // parado, marcha lenta
    {30000, 0, 800, 15, 20, 30},      // começa a acelerar
    {50000, 80, 3000, 50, 75, 85},    // fim da aceleração
    {55000, 80, 2200, 25, 40, 50},    // cruzeiro
    {200000, 80, 2200, 25, 40, 50},   // começa a frear
    {220000, 0, 800, 15, 15, 25},     // parou
    {225000, 0, 800, 15, 20, 30},     // marcha lenta
    {duracao_da_viagem_ms, 0, 800, 15, 20, 30},  // volta ao início
};
constexpr size_t pontos = sizeof(perfil) / sizeof(perfil[0]);
static_assert(perfil[0].ms == 0 && perfil[pontos - 1].ms == duracao_da_viagem_ms,
              "O perfil cobre a viagem inteira");

constexpr float coolant_inicial = 47;
constexpr float coolant_final = 90;
constexpr uint32_t aquecimento_ms = 600000;
constexpr float iat = 30;
constexpr float voltage = 13.8f;

constexpr uint8_t enchimento = 0x55;

float entre(float a, float b, float fracao) { return a + (b - a) * fracao; }

uint8_t byte_de(float valor) {
  const long arredondado = std::lround(valor);
  return static_cast<uint8_t>(arredondado < 0 ? 0 : arredondado > 255 ? 255 : arredondado);
}

uint16_t palavra_de(float valor) {
  const long arredondado = std::lround(valor);
  return static_cast<uint16_t>(arredondado < 0 ? 0 : arredondado > 0xFFFF ? 0xFFFF : arredondado);
}

// Os bytes de dados do PID, no inverso das fórmulas do decodificar(). Devolve quantos (1 ou 2),
// ou 0 se a central não tem o PID.
uint8_t dados_do_pid(uint8_t pid, const Viagem &v, uint8_t (&dados)[2]) {
  switch (pid) {
    case 0x04: dados[0] = byte_de(v.load * 255 / 100); return 1;
    case 0x05: dados[0] = byte_de(v.coolant + 40); return 1;
    case 0x0B: dados[0] = byte_de(v.map); return 1;
    case 0x0D: dados[0] = byte_de(v.speed); return 1;
    case 0x0F: dados[0] = byte_de(v.iat + 40); return 1;
    case 0x11: dados[0] = byte_de(v.throttle * 255 / 100); return 1;
    case 0x0C:
    case 0x42: {
      const uint16_t bruto = palavra_de(pid == 0x0C ? v.rpm * 4 : v.voltage * 1000);
      dados[0] = static_cast<uint8_t>(bruto >> 8);
      dados[1] = static_cast<uint8_t>(bruto);
      return 2;
    }
    default: return 0;
  }
}

}  // namespace

Viagem viagem(uint32_t ms_desde_o_boot) {
  const uint32_t no_laco = ms_desde_o_boot % duracao_da_viagem_ms;
  size_t i = 0;
  while (i + 2 < pontos && perfil[i + 1].ms <= no_laco) {
    ++i;
  }
  const Ponto &a = perfil[i];
  const Ponto &b = perfil[i + 1];
  const float fracao = static_cast<float>(no_laco - a.ms) / static_cast<float>(b.ms - a.ms);
  const uint32_t aquecendo = ms_desde_o_boot < aquecimento_ms ? ms_desde_o_boot : aquecimento_ms;

  Viagem v{};
  v.speed = entre(a.speed, b.speed, fracao);
  v.rpm = entre(a.rpm, b.rpm, fracao);
  v.throttle = entre(a.throttle, b.throttle, fracao);
  v.load = entre(a.load, b.load, fracao);
  v.map = entre(a.map, b.map, fracao);
  v.coolant = entre(coolant_inicial, coolant_final,
                    static_cast<float>(aquecendo) / static_cast<float>(aquecimento_ms));
  v.iat = iat;
  v.voltage = voltage;
  return v;
}

bool responder(uint16_t id, const uint8_t (&pedido)[8], uint32_t ms_desde_o_boot,
               Quadro &resposta) {
  if ((id != id_todos && id != id_central) || pedido[0] != 2 || pedido[1] != 1) {
    return false;
  }
  const uint8_t pid = pedido[2];
  resposta.id = id_resposta;
  resposta.dlc = 8;
  std::memset(resposta.dados, enchimento, sizeof(resposta.dados));
  resposta.dados[1] = 0x41;
  resposta.dados[2] = pid;

  if (pid == 0x00 || pid == 0x20 || pid == 0x40) {
    const uint32_t valor = mapas_simulados[pid / 0x20];
    resposta.dados[0] = 6;
    for (uint8_t i = 0; i < 4; ++i) {
      resposta.dados[3 + i] = static_cast<uint8_t>(valor >> (24 - 8 * i));
    }
    return true;
  }
  uint8_t dados[2] = {};
  const uint8_t quantidade = dados_do_pid(pid, viagem(ms_desde_o_boot), dados);
  if (quantidade == 0) {
    return false;
  }
  resposta.dados[0] = static_cast<uint8_t>(2 + quantidade);
  std::memcpy(resposta.dados + 3, dados, quantidade);
  return true;
}

}  // namespace vguard::obd
