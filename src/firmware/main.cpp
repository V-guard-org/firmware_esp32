// Firmware do ESP32 do V-GUARD (proposta §4). Por enquanto, ele se apresenta pela USB ($B ao
// ligar e um $S por segundo, #2) e obedece à Pi: o $H o põe em ATIVO e o $C,obd_hz muda a taxa
// do OBD (#3). O OBD, o acelerômetro e o SD vêm nos tickets seguintes.
#include <Arduino.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "protocolo.h"
#include "serial_pi.h"

static_assert(__cplusplus >= 201703L, "o firmware usa C++17 (proposta §4.8)");

namespace {

using vguard::protocolo::EstadoImu;
using vguard::protocolo::EstadoObd;
using vguard::protocolo::EstadoSd;
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
  status.obd = EstadoObd::parado;
  status.imu = EstadoImu::falha;
  status.sd = EstadoSd::ausente;
  status.sd_livre = 0;
  return status;
}

// Segundos desde o início das tarefas, um a mais por $S.
uint32_t uptime_s = 0;
// Último despertar do loop(). Começando em 0, o $S sai em 1 s, 2 s, 3 s... sem acumular atraso.
TickType_t ultimo_status = 0;

}  // namespace

void setup() {
  // Na USB, só as linhas do protocolo: um aviso do ESP-IDF no meio de uma linha a corromperia.
  esp_log_level_set("*", ESP_LOG_NONE);
  vguard::serial_pi::iniciar();
  vguard::serial_pi::emitir_boot(motivo_do_reset());
  vguard::serial_pi::emitir_status(status_atual(uptime_s));
}

void loop() {
  vTaskDelayUntil(&ultimo_status, pdMS_TO_TICKS(vguard::config::periodo_status_ms));
  ++uptime_s;
  vguard::serial_pi::emitir_status(status_atual(uptime_s));
}
