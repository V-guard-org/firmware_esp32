#include "aquisicao.h"

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdlib>

#include "adxl345.h"
#include "can_mcp2515.h"
#include "config.h"
#include "imu.h"
#include "obd.h"
#include "serial_pi.h"

namespace vguard::aquisicao {

namespace {

using protocolo::EstadoObd;
using protocolo::Nivel;

TickType_t ticks(uint32_t ms) { return pdMS_TO_TICKS(ms); }

// Os ms desde o boot, como o millis(): dão a volta em ~49 dias, e a máquina do OBD aguenta.
uint32_t agora_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

// --- Acelerômetro ---

// Sem calibração gravada, offset zero (proposta §4.4). A calibração pelo $K vem no #5.
constexpr imu::Aceleracao offset{};

// O sensor respondeu à última inicialização ou leitura.
std::atomic<bool> respondendo{false};
// O resultado da conferência de 1 g. Só vale com a conferência feita.
std::atomic<bool> dentro_da_faixa{false};
// Só o setup() e depois a tarefa usam.
bool conferencia_feita = false;
imu::Tratamento tratamento;
bool avisou_calibracao = false;
TickType_t ultima_tentativa = 0;

// As 10 leituras da conferência de 1 g, uma a cada 10 ms. Devolve false se o sensor falhou no
// meio, e aí a conferência fica por fazer.
bool conferir() {
  imu::Leitura leituras[imu::leituras_conferencia];
  vTaskDelay(ticks(config::imu_espera_primeira_amostra_ms));
  for (size_t i = 0; i < imu::leituras_conferencia; ++i) {
    if (i > 0) {
      vTaskDelay(ticks(config::aquisicao_periodo_ms));
    }
    if (!adxl345::ler(leituras[i])) {
      return false;
    }
  }
  dentro_da_faixa = imu::conferir_1g(leituras, imu::leituras_conferencia, offset);
  conferencia_feita = true;
  return true;
}

// Destrava o barramento e inicia o sensor. Faz a conferência se ela ainda não foi feita, e o
// filtro recomeça na próxima leitura. Devolve false se o sensor não respondeu.
bool religar() {
  if (!adxl345::iniciar()) {
    return false;
  }
  if (!conferencia_feita && !conferir()) {
    return false;
  }
  tratamento.recomecar();
  return true;
}

// Um ciclo do acelerômetro em ATIVO. Devolve true se ele demorou (uma tentativa ou um
// destravamento): o ritmo recomeça dali, em vez de correr atrás das leituras perdidas.
bool ciclo_do_imu() {
  if (!avisou_calibracao) {
    serial_pi::emitir_log(Nivel::aviso, "IMU sem calibracao");
    avisou_calibracao = true;
  }
  if (!respondendo) {
    const TickType_t agora = xTaskGetTickCount();
    if (agora - ultima_tentativa < ticks(config::imu_intervalo_tentativas_ms)) {
      return false;
    }
    ultima_tentativa = agora;
    if (religar()) {
      respondendo = true;
      serial_pi::emitir_log(Nivel::aviso, "IMU voltou");
    }
    return true;
  }
  imu::Leitura leitura;
  if (!adxl345::ler(leitura)) {
    // Erro de leitura: destrava e inicia de novo, na hora. Se voltar, só se perdem estas
    // leituras, sem aviso.
    if (!religar()) {
      respondendo = false;
      ultima_tentativa = xTaskGetTickCount();
      serial_pi::emitir_log(Nivel::erro, "IMU sem resposta");
    }
    return true;
  }
  imu::Aceleracao aceleracao;
  if (tratamento.receber(leitura, aceleracao)) {
    serial_pi::emitir_acelerometro(imu::centesimos(aceleracao.x), imu::centesimos(aceleracao.y),
                                   imu::centesimos(aceleracao.z));
  }
  return false;
}

// --- OBD ---

// O MCP2515: ainda não iniciado (ESPERA), iniciado ou sem resposta.
enum class Can : uint8_t { nao_iniciado, pronto, falhou };
Can can = Can::nao_iniciado;
// O obd do $S, lido pelo loop().
std::atomic<EstadoObd> obd_no_status{EstadoObd::parado};

// O que a máquina do OBD pede: o MCP2515 e a serial.
class SaidaDoObd final : public obd::Saida {
 public:
  void configurar_leitura() override {
    // Sem resposta do MCP2515: obd FALHA até o próximo boot. As regras de voltar a escutar vêm
    // no #7.
    if (!can_mcp2515::configurar_leitura()) {
      can = Can::falhou;
    }
  }
  void enviar(uint16_t id, const uint8_t (&dados)[8]) override {
    // O quadro que não saiu fica sem resposta, e o tempo limite cuida dele.
    can_mcp2515::enviar(id, dados);
  }
  void emitir_obd(const char *campo, int64_t valor_escalado, uint8_t casas) override {
    serial_pi::emitir_obd(campo, valor_escalado, casas);
  }
  void emitir_pids(uint32_t mapa00, uint32_t mapa20, uint32_t mapa40) override {
    serial_pi::emitir_pids(mapa00, mapa20, mapa40);
  }
};

SaidaDoObd saida_do_obd;
obd::Maquina maquina(saida_do_obd);

// Um ciclo do OBD em ATIVO: inicia o MCP2515 na primeira vez, passa os quadros recebidos à
// máquina e chama o tique.
void ciclo_do_obd() {
  const uint32_t agora = agora_ms();
  if (can == Can::nao_iniciado) {
    if (can_mcp2515::iniciar_escuta()) {
      can = Can::pronto;
      maquina.comecar(agora);
    } else {
      can = Can::falhou;
    }
  }
  if (can == Can::pronto) {
    obd::Quadro quadro;
    for (uint32_t n = 0; n < config::obd_quadros_por_ciclo && can_mcp2515::receber(quadro); ++n) {
      maquina.receber(quadro, agora);
    }
    maquina.tique(agora, serial_pi::obd_hz_decimos());
  }
  obd_no_status = can == Can::pronto ? maquina.estado() : EstadoObd::falha;
}

void tarefa(void *) {
  if (!respondendo) {
    serial_pi::emitir_log(Nivel::erro, "IMU sem resposta");
  }
  ultima_tentativa = xTaskGetTickCount();
  TickType_t despertar = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&despertar, ticks(config::aquisicao_periodo_ms));
    // Em ESPERA, nem o CAN nem o sensor são lidos.
    if (!serial_pi::ativo()) {
      continue;
    }
    ciclo_do_obd();
    if (ciclo_do_imu()) {
      despertar = xTaskGetTickCount();
    }
  }
}

}  // namespace

void preparar() {
  can_mcp2515::preparar();
  tratamento.definir_offset(offset);
  adxl345::instalar();
  respondendo = religar();
}

void iniciar() {
  // Sem memória já no setup(), seguir sem a aquisição não serve: reinicia (PANE no $B).
  if (xTaskCreatePinnedToCore(tarefa, "aquisicao", config::aquisicao_pilha, nullptr,
                              config::aquisicao_prioridade, nullptr,
                              config::aquisicao_nucleo) != pdPASS) {
    abort();
  }
}

protocolo::EstadoImu estado_imu() {
  return respondendo && dentro_da_faixa ? protocolo::EstadoImu::ok : protocolo::EstadoImu::falha;
}

protocolo::EstadoObd estado_obd() { return obd_no_status; }

}  // namespace vguard::aquisicao
