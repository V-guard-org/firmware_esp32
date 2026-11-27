// CAN + ADXL345 + microSD + USB/serial para a Raspberry Pi.
// Abra esta pasta inteira no Arduino IDE. ESP32 DevKit V1, pacote esp32 3.3.12.
// config.h: pinos e taxas | aquisicao.cpp: sensores | saida.cpp: serial e SD.
// src/nucleo: protocolo, OBD, filtro da IMU e formato do SD originais.
#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdlib>
#include "config.h"
#include "interfaces.h"

static_assert(__cplusplus >= 201703L, "Use o pacote ESP32 3.x no Arduino IDE.");
static_assert(configNUMBER_OF_CORES == 2, "Este firmware exige ESP32 de dois nucleos.");

vguard::protocolo::MotivoReset motivoDoReset() {
  using vguard::protocolo::MotivoReset;
  switch (esp_reset_reason()) {
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

vguard::protocolo::Status statusAtual(uint32_t uptime) {
  vguard::protocolo::Status status{};
  status.uptime_s = uptime;
  status.obd = vguard::aquisicao::estado_obd();
  status.imu = vguard::aquisicao::estado_imu();
  status.sd = vguard::gravacao_sd::estado();
  status.sd_livre = vguard::gravacao_sd::livre();
  return status;
}

// O status periódico também roda no núcleo 0, sem depender do loop do Arduino.
void tarefaStatus(void *) {
  TickType_t despertar = 0;
  uint32_t uptime = 0;
  for (;;) {
    vTaskDelayUntil(&despertar, pdMS_TO_TICKS(vguard::config::periodo_status_ms));
    vguard::serial_pi::emitir_status(statusAtual(++uptime));
  }
}

void setup() {
  esp_log_level_set("*", ESP_LOG_NONE);
  // Deixa os dois dispositivos SPI desmarcados antes de inicializar os barramentos.
  pinMode(vguard::pinos::can_cs, OUTPUT);
  pinMode(vguard::pinos::sd_cs, OUTPUT);
  digitalWrite(vguard::pinos::can_cs, HIGH);
  digitalWrite(vguard::pinos::sd_cs, HIGH);

  // Inicialização única; a divisão por núcleo vale para as tarefas abaixo.
  vguard::serial_pi::iniciar();
  vguard::gravacao_sd::preparar();
  vguard::aquisicao::preparar();
  vguard::serial_pi::emitir_boot(motivoDoReset());
  vguard::serial_pi::emitir_status(statusAtual(0));
  vguard::aquisicao::iniciar();
  vguard::gravacao_sd::iniciar();
  if (xTaskCreatePinnedToCore(tarefaStatus, "status_pi", vguard::config::status_pilha,
                             nullptr, vguard::config::status_prioridade, nullptr,
                             vguard::config::saida_nucleo) != pdPASS) {
    abort();
  }
}

void loop() {
  // Todo o trabalho contínuo está nas tarefas FreeRTOS.
  vTaskSuspend(nullptr);
}