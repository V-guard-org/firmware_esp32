#include "serial_pi.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <cstdlib>

#include "config.h"

namespace vguard::serial_pi {

namespace {

StreamBufferHandle_t saida = nullptr;
SemaphoreHandle_t trava = nullptr;

// O buffer na RAM, visto pelo Emissor do núcleo como o destino das linhas.
class BufferDeSaida final : public protocolo::Destino {
 public:
  bool escrever(const char *linha, size_t tamanho) override {
    // Com a trava, só um escreve por vez: o espaço visto aqui não diminui antes do envio, e
    // a linha entra inteira ou não entra.
    if (xStreamBufferSpacesAvailable(saida) < tamanho) {
      return false;
    }
    return xStreamBufferSend(saida, linha, tamanho, 0) == tamanho;
  }
};

BufferDeSaida buffer_de_saida;
protocolo::Emissor emissor(buffer_de_saida);

// Segura a trava do Emissor até o fim do bloco. A espera é só pela outra tarefa que estiver
// emitindo, e isso leva microssegundos: montar a linha e copiá-la para o buffer.
class ComTrava {
 public:
  ComTrava() { xSemaphoreTake(trava, portMAX_DELAY); }
  ~ComTrava() { xSemaphoreGive(trava); }
  ComTrava(const ComTrava &) = delete;
  ComTrava &operator=(const ComTrava &) = delete;
};

// Passa o buffer para a UART. Esta tarefa pode esperar a UART; quem emite, não.
void tarefa_escrita(void *) {
  uint8_t bloco[128];
  for (;;) {
    const size_t quantidade = xStreamBufferReceive(saida, bloco, sizeof(bloco), portMAX_DELAY);
    Serial.write(bloco, quantidade);
  }
}

}  // namespace

void iniciar() {
  Serial.begin(config::serial_baud, SERIAL_8N1);
  // Sem memória já no setup(), seguir sem saída serial não serve: reinicia (PANE no $B).
  saida = xStreamBufferCreate(config::serial_tamanho_saida, 1);
  trava = xSemaphoreCreateMutex();
  if (saida == nullptr || trava == nullptr) {
    abort();
  }
  if (xTaskCreatePinnedToCore(tarefa_escrita, "serial_pi", config::serial_pilha_escrita, nullptr,
                              config::serial_prioridade_escrita, nullptr,
                              tskNO_AFFINITY) != pdPASS) {
    abort();
  }
}

void emitir_boot(uint64_t t, protocolo::MotivoReset motivo) {
  ComTrava com_trava;
  emissor.emitir_boot(t, versao_firmware, motivo);
}

void emitir_status(uint64_t t, const protocolo::Status &status) {
  ComTrava com_trava;
  emissor.emitir_status(t, status);
}

}  // namespace vguard::serial_pi
