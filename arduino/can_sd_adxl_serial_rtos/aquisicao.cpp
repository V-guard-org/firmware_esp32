// NUCLEO 1: MCP2515 + ADXL345. Sem acesso ao SD ou escrita na UART.
#include "interfaces.h"

// ==================== can_mcp2515 ====================

#include <Arduino.h>
#include <SPI.h>
#include "src/mcp_can/mcp_can.h"

#include <cstring>

#include "config.h"

namespace vguard::can {

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

bool erro_no_barramento() {
  return (controlador.getError() & (MCP_EFLG_TXBO | MCP_EFLG_TXEP | MCP_EFLG_RXEP)) != 0;
}

}  // namespace vguard::can

// ==================== adxl345 ====================

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <esp_rom_gpio.h>
#include <esp_rom_sys.h>
#include <soc/gpio_sig_map.h>

#include <cstdlib>

#include "config.h"

namespace vguard::adxl345 {

namespace {

constexpr i2c_port_t porta = I2C_NUM_0;
constexpr gpio_num_t sda = static_cast<gpio_num_t>(pinos::imu_sda);
constexpr gpio_num_t scl = static_cast<gpio_num_t>(pinos::imu_scl);

// Registradores e valores (proposta §4.4).
constexpr uint8_t registrador_devid = 0x00;
constexpr uint8_t devid_esperado = 0xE5;
constexpr uint8_t registrador_bw_rate = 0x2C;
constexpr uint8_t bw_rate_100_hz = 0x0A;
constexpr uint8_t registrador_power_ctl = 0x2D;
constexpr uint8_t power_ctl_medir = 0x08;
constexpr uint8_t registrador_data_format = 0x31;
constexpr uint8_t data_format_16g_resolucao_completa = 0x0B;
// DATAX0: os 6 bytes dos três eixos começam aqui, o byte baixo antes do alto.
constexpr uint8_t registrador_datax0 = 0x32;

// No destravamento, o SCL pulsa a 100 kHz, mais devagar que os 400 kHz do driver.
constexpr uint32_t meio_periodo_us = 5;
constexpr int maximo_pulsos = 9;

TickType_t espera() { return pdMS_TO_TICKS(config::imu_i2c_espera_ms); }

bool escrever(uint8_t registrador, uint8_t valor) {
  const uint8_t dados[] = {registrador, valor};
  return i2c_master_write_to_device(porta, endereco_imu, dados, sizeof(dados), espera()) ==
         ESP_OK;
}

bool ler_registradores(uint8_t primeiro, uint8_t *dados, size_t quantidade) {
  return i2c_master_write_read_device(porta, endereco_imu, &primeiro, 1, dados, quantidade,
                                      espera()) == ESP_OK;
}

// O pino vira um GPIO de dreno aberto, solto: o pull-up o leva ao nível alto.
void tomar_do_controlador(gpio_num_t pino) {
  gpio_set_level(pino, 1);
  gpio_set_direction(pino, GPIO_MODE_INPUT_OUTPUT_OD);
  esp_rom_gpio_connect_out_signal(pino, SIG_GPIO_OUT_IDX, false, false);
}

void nivel(gpio_num_t pino, uint32_t valor) {
  gpio_set_level(pino, valor);
  esp_rom_delay_us(meio_periodo_us);
}

void destravar() {
  tomar_do_controlador(sda);
  tomar_do_controlador(scl);
  esp_rom_delay_us(meio_periodo_us);
  // A cada pulso, o sensor que prende o SDA avança um bit; em até 9, ele solta.
  for (int pulso = 0; pulso < maximo_pulsos && gpio_get_level(sda) == 0; ++pulso) {
    nivel(scl, 0);
    nivel(scl, 1);
  }
  // STOP à mão: com o SCL alto, o SDA sobe.
  nivel(scl, 0);
  nivel(sda, 0);
  nivel(scl, 1);
  nivel(sda, 1);
  // Devolve os pinos ao controlador I²C, com a fila dele vazia.
  i2c_set_pin(porta, sda, scl, GPIO_PULLUP_ENABLE, GPIO_PULLUP_ENABLE, I2C_MODE_MASTER);
  i2c_reset_tx_fifo(porta);
  i2c_reset_rx_fifo(porta);
}

int16_t eixo(const uint8_t *dados) {
  return static_cast<int16_t>(static_cast<uint16_t>(dados[1]) << 8 | dados[0]);
}

}  // namespace

void instalar() {
  i2c_config_t configuracao{};
  configuracao.mode = I2C_MODE_MASTER;
  configuracao.sda_io_num = sda;
  configuracao.scl_io_num = scl;
  // O módulo pode já ter pull-ups; os internos, de ~45 kΩ, só somam.
  configuracao.sda_pullup_en = GPIO_PULLUP_ENABLE;
  configuracao.scl_pullup_en = GPIO_PULLUP_ENABLE;
  configuracao.master.clk_speed = config::imu_i2c_hz;
  // Sem o driver, não há acelerômetro: é erro do firmware, não do sensor. Reinicia (PANE no $B).
  if (i2c_param_config(porta, &configuracao) != ESP_OK ||
      i2c_driver_install(porta, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
    abort();
  }
}

bool iniciar() {
  destravar();
  uint8_t devid = 0;
  return ler_registradores(registrador_devid, &devid, 1) && devid == devid_esperado &&
         escrever(registrador_data_format, data_format_16g_resolucao_completa) &&
         escrever(registrador_bw_rate, bw_rate_100_hz) &&
         escrever(registrador_power_ctl, power_ctl_medir);
}

bool ler(imu::Leitura &leitura) {
  uint8_t dados[6];
  if (!ler_registradores(registrador_datax0, dados, sizeof(dados))) {
    return false;
  }
  leitura.x = eixo(dados);
  leitura.y = eixo(dados + 2);
  leitura.z = eixo(dados + 4);
  return true;
}

}  // namespace vguard::adxl345

// ==================== acelerometro_adxl345 ====================
// O acelerômetro do ambiente esp32 (o do carro): sempre o ADXL345 de verdade.

namespace vguard::acelerometro {

void instalar() { adxl345::instalar(); }

bool iniciar() { return adxl345::iniciar(); }

bool ler(imu::Leitura &leitura) { return adxl345::ler(leitura); }

bool simulado() { return false; }

}  // namespace vguard::acelerometro

// ==================== aquisicao ====================

#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdlib>

#include "config.h"
#include "src/nucleo/imu.h"
#include "src/nucleo/obd.h"

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
bool avisou_simulado = false;
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
    if (!acelerometro::ler(leituras[i])) {
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
  if (!acelerometro::iniciar()) {
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
  // No esp32-sim sem o sensor, as leituras são as do simulado (#19).
  if (!avisou_simulado && acelerometro::simulado()) {
    serial_pi::emitir_log(Nivel::aviso, "IMU simulado");
    avisou_simulado = true;
  }
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
  if (!acelerometro::ler(leitura)) {
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

// O MCP2515: ainda não iniciado (ESPERA), iniciado ou sem resposta. Sem resposta, fica assim até
// o próximo boot (obd FALHA).
enum class Can : uint8_t { nao_iniciado, pronto, falhou };
Can estado_can = Can::nao_iniciado;
// O obd do $S, lido pelo loop().
std::atomic<EstadoObd> obd_no_status{EstadoObd::parado};

// O que a máquina do OBD pede: o MCP2515 e a serial.
class SaidaDoObd final : public obd::Saida {
 public:
  bool configurar_leitura() override { return can::configurar_leitura(); }
  bool voltar_a_escutar() override { return can::iniciar_escuta(); }
  void enviar(uint16_t id, const uint8_t (&dados)[8]) override {
    // O quadro que não saiu fica sem resposta, e o tempo limite cuida dele.
    can::enviar(id, dados);
  }
  void emitir_obd(const char *campo, int64_t valor_escalado, uint8_t casas) override {
    serial_pi::emitir_obd(campo, valor_escalado, casas);
  }
  void emitir_pids(uint32_t mapa00, uint32_t mapa20, uint32_t mapa40) override {
    serial_pi::emitir_pids(mapa00, mapa20, mapa40);
  }
  void emitir_aviso(const char *texto) override { serial_pi::emitir_log(Nivel::aviso, texto); }
};

SaidaDoObd saida_do_obd;
obd::Maquina maquina(saida_do_obd);

// Um ciclo do OBD em ATIVO: inicia o MCP2515 na primeira vez, passa os quadros recebidos à
// máquina, confere os erros do CAN enquanto ela pede e chama o tique.
void ciclo_do_obd() {
  const uint32_t agora = agora_ms();
  if (estado_can == Can::nao_iniciado) {
    if (can::iniciar_escuta()) {
      estado_can = Can::pronto;
      maquina.comecar(agora);
    } else {
      estado_can = Can::falhou;
    }
  }
  if (estado_can == Can::pronto) {
    obd::Quadro quadro;
    for (uint32_t n = 0; n < config::obd_quadros_por_ciclo && can::receber(quadro); ++n) {
      maquina.receber(quadro, agora);
    }
    if (maquina.pedindo() && can::erro_no_barramento()) {
      maquina.erro_no_can(agora);
    }
    maquina.tique(agora, serial_pi::obd_hz_decimos());
    // O MCP2515 não voltou a só escuta: para de falar com ele.
    if (maquina.estado() == EstadoObd::falha) {
      estado_can = Can::falhou;
    }
  }
  obd_no_status = estado_can == Can::pronto ? maquina.estado() : EstadoObd::falha;
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
  can::preparar();
  tratamento.definir_offset(offset);
  acelerometro::instalar();
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
