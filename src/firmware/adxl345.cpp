#include "adxl345.h"

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
