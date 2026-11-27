// Tratamento do acelerômetro ADXL345 (proposta §4.4): o offset, a conversão para m/s², o
// passa-baixa de 5 Hz, uma amostra de cada duas e a conferência de 1 g. Só memória de tamanho
// fixo.
//
// A parte do ESP32 lê o sensor a 100 Hz e passa cada leitura crua ao Tratamento, que devolve o
// $A a 50 Hz, já filtrado, com a gravidade incluída.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vguard::imu {

// Escala com DATA_FORMAT = 0x0B (±16 g, resolução completa): ~3,9 mg/LSB em qualquer faixa.
constexpr float g_por_lsb = 0.0039f;
// Gravidade padrão, em m/s².
constexpr float gravidade = 9.80665f;
// Passa-baixa EMA a 100 Hz com corte em ~5 Hz: α = 1 − e^(−2π·5/100) ≈ 0,2696. O teste confere
// a conta.
constexpr float alfa = 0.2696f;

// Conferência de 1 g: o |a| da média das leituras, entre 0,8 e 1,2 g.
constexpr float conferencia_minimo_g = 0.8f;
constexpr float conferencia_maximo_g = 1.2f;
// Leituras da conferência: 0,1 s a 100 Hz.
constexpr size_t leituras_conferencia = 10;

// Uma leitura crua dos três eixos, em LSB, como o sensor entrega.
struct Leitura {
  int16_t x;
  int16_t y;
  int16_t z;
};

// Aceleração dos três eixos, em m/s².
struct Aceleracao {
  float x;
  float y;
  float z;
};

// A leitura crua em m/s², menos o offset (também em m/s²).
Aceleracao em_ms2(Leitura crua, Aceleracao offset);

// O valor em centésimos, arredondado para o mais próximo: o $A tem 2 casas. 9.786 dá 979, e
// −0.054 dá −5.
int32_t centesimos(float valor);

// Confere que o |a| da média das leituras, já sem o offset, fica entre 0,8 e 1,2 g. Sem
// leituras, não confere.
bool conferir_1g(const Leitura *leituras, size_t quantidade, Aceleracao offset);

// Offset, filtro e uma amostra de cada duas, para as leituras a 100 Hz.
class Tratamento {
 public:
  // O valor subtraído da leitura, em m/s². Zero até a calibração (#5).
  void definir_offset(Aceleracao offset) { offset_ = offset; }

  // Recomeça o filtro, depois de cada volta do sensor: a próxima leitura entra direto como a
  // saída, em vez de o filtro subir de 0 até ela.
  void recomecar() { comecou_ = false; }

  // Recebe uma leitura crua a 100 Hz. Devolve true em uma de cada duas, a começar pela
  // primeira depois do recomecar(), com a aceleração filtrada em `saida`.
  bool receber(Leitura crua, Aceleracao &saida);

 private:
  Aceleracao offset_{};
  Aceleracao filtrada_{};
  bool comecou_ = false;
  // A próxima leitura filtrada vai para a saída.
  bool sai_a_proxima_ = true;
};

}  // namespace vguard::imu
