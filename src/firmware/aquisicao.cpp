#include "aquisicao.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdlib>

#include "adxl345.h"
#include "config.h"
#include "imu.h"
#include "serial_pi.h"

namespace vguard::aquisicao {

namespace {

using protocolo::Nivel;

// Sem calibração gravada, offset zero (proposta §4.4). A calibração pelo $K vem no #5.
constexpr imu::Aceleracao offset{};

// O sensor respondeu à última inicialização ou leitura.
std::atomic<bool> respondendo{false};
// O resultado da conferência de 1 g. Só vale com a conferência feita.
std::atomic<bool> dentro_da_faixa{false};
// Só o setup() e depois a tarefa usam.
bool conferencia_feita = false;
imu::Tratamento tratamento;

TickType_t ticks(uint32_t ms) { return pdMS_TO_TICKS(ms); }

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

void tarefa(void *) {
  if (!respondendo) {
    serial_pi::emitir_log(Nivel::erro, "IMU sem resposta");
  }
  bool avisou_calibracao = false;
  TickType_t ultima_tentativa = xTaskGetTickCount();
  TickType_t despertar = xTaskGetTickCount();
  for (;;) {
    vTaskDelayUntil(&despertar, ticks(config::aquisicao_periodo_ms));
    // Em ESPERA, o sensor não é lido.
    if (!serial_pi::ativo()) {
      continue;
    }
    if (!avisou_calibracao) {
      serial_pi::emitir_log(Nivel::aviso, "IMU sem calibracao");
      avisou_calibracao = true;
    }

    if (!respondendo) {
      const TickType_t agora = xTaskGetTickCount();
      if (agora - ultima_tentativa < ticks(config::imu_intervalo_tentativas_ms)) {
        continue;
      }
      ultima_tentativa = agora;
      if (religar()) {
        respondendo = true;
        serial_pi::emitir_log(Nivel::aviso, "IMU voltou");
      }
      // A tentativa pode levar uns 0,1 s (a conferência): o ritmo recomeça daqui, em vez de
      // correr atrás das leituras perdidas.
      despertar = xTaskGetTickCount();
      continue;
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
      despertar = xTaskGetTickCount();
      continue;
    }
    imu::Aceleracao aceleracao;
    if (tratamento.receber(leitura, aceleracao)) {
      serial_pi::emitir_acelerometro(imu::centesimos(aceleracao.x),
                                     imu::centesimos(aceleracao.y),
                                     imu::centesimos(aceleracao.z));
    }
  }
}

}  // namespace

void preparar() {
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

}  // namespace vguard::aquisicao
