// Firmware do ESP32 do V-GUARD (proposta §4). Por enquanto, ele se apresenta pela USB ($B ao
// ligar e um $S por segundo, #2), obedece à Pi (o $H o põe em ATIVO e o $C,obd_hz muda a taxa
// do OBD, #3), em ATIVO manda o acelerômetro a 50 Hz ($A, #4) e o OBD ($P e $O, #6), e grava
// tudo no microSD, um arquivo por hora (#8). Sem $H por 120 s, entra em ATIVO sozinho, no modo
// autônomo (#21).
#include <Arduino.h>

#include <cstdlib>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "aquisicao.h"
#include "config.h"
#include "gravacao_sd.h"
#include "protocolo.h"
#include "serial_pi.h"

static_assert(__cplusplus >= 201703L, "o firmware usa C++17 (proposta §4.8)");

namespace {

using vguard::protocolo::MotivoReset;
using vguard::protocolo::Status;

// O motivo que o ESP32 informa, nos nomes do $B (proposta §5.3).
MotivoReset motivo_do_reset() {
  switch (esp_reset_reason()) {
    // No ESP32 clássico, o botão EN e o reinício pela USB também aparecem como ligar.
    case ESP_RST_POWERON: return MotivoReset::ligou;
    case ESP_RST_SW: return MotivoReset::software;
    case ESP_RST_PANIC: return MotivoReset::pane;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT: return MotivoReset::watchdog;
    case ESP_RST_BROWNOUT: return MotivoReset::queda_tensao;
    default: return MotivoReset::outro;
  }
}

// As peças que ainda não existem saem com valores que não fingem funcionar (proposta §4.8).
// O estado e o obd_hz, a serial_pi preenche com o $H e o $C recebidos.
Status status_atual(uint32_t uptime_s) {
  Status status{};
  status.uptime_s = uptime_s;
  status.obd = vguard::aquisicao::estado_obd();
  status.imu = vguard::aquisicao::estado_imu();
  status.sd = vguard::gravacao_sd::estado();
  status.sd_livre = vguard::gravacao_sd::livre();
  return status;
}

// Segundos desde o início das tarefas, um a mais por $S.
uint32_t uptime_s = 0;

// O $S de 1 Hz, no núcleo 0 com a serial e o SD, longe da aquisição (proposta §4.6, #21).
// Começando em 0, o $S sai em 1 s, 2 s, 3 s... sem acumular atraso.
void tarefa_status(void *) {
  TickType_t ultimo_status = 0;
  for (;;) {
    vTaskDelayUntil(&ultimo_status, pdMS_TO_TICKS(vguard::config::periodo_status_ms));
    ++uptime_s;
    vguard::serial_pi::emitir_status(status_atual(uptime_s));
  }
}

}  // namespace

void setup() {
  // Na USB, só as linhas do protocolo: um aviso do ESP-IDF no meio de uma linha a corromperia.
  esp_log_level_set("*", ESP_LOG_NONE);
  // Os dois aparelhos SPI desmarcados antes de iniciar os barramentos (#21).
  pinMode(vguard::pinos::can_cs, OUTPUT);
  pinMode(vguard::pinos::sd_cs, OUTPUT);
  digitalWrite(vguard::pinos::can_cs, HIGH);
  digitalWrite(vguard::pinos::sd_cs, HIGH);
  vguard::serial_pi::iniciar();
  // O cartão é montado antes do $B: o $B já vai para a fila do SD, e o primeiro $S sai com o sd
  // certo.
  vguard::gravacao_sd::preparar();
  // A conferência de 1 g vem antes do $B (~0,15 s), para o primeiro $S já sair com o imu certo.
  vguard::aquisicao::preparar();
  vguard::serial_pi::emitir_boot(motivo_do_reset());
  vguard::serial_pi::emitir_status(status_atual(uptime_s));
  vguard::aquisicao::iniciar();
  vguard::gravacao_sd::iniciar();
  if (xTaskCreatePinnedToCore(tarefa_status, "status_pi", vguard::config::status_pilha, nullptr,
                              vguard::config::status_prioridade, nullptr,
                              vguard::config::saida_nucleo) != pdPASS) {
    abort();
  }
}

// Todo o trabalho contínuo está nas tarefas.
void loop() { vTaskSuspend(nullptr); }
