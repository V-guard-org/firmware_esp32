#include "serial_pi.h"

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <cstdlib>

#include "config.h"
#include "gravacao_sd.h"

namespace vguard::serial_pi {

namespace {

StreamBufferHandle_t saida = nullptr;
// Protege o Emissor, o relógio e a taxa do OBD, que as tarefas dividem.
SemaphoreHandle_t trava = nullptr;

// O buffer na RAM, visto pelo Emissor do núcleo como o destino das linhas. Cada linha também vai
// para a fila do SD, mesmo a que não couber na serial (proposta §4.5).
class BufferDeSaida final : public protocolo::Destino {
 public:
  bool escrever(const char *linha, size_t tamanho) override {
    gravacao_sd::enfileirar(linha, tamanho);
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
protocolo::Relogio relogio;
uint16_t obd_hz_em_uso = config::obd_hz_inicial_decimos;
// Só a tarefa de leitura usa.
protocolo::Leitor leitor;

// Tempo desde o boot, em ms. O contador do esp_timer tem 64 bits e não dá a volta.
uint64_t agora_ms() { return static_cast<uint64_t>(esp_timer_get_time()) / 1000; }

// Segura a trava até o fim do bloco. A espera é só pela outra tarefa que estiver emitindo ou
// aplicando um comando da Pi, e isso leva microssegundos: montar a linha e copiá-la para o
// buffer, ou trocar dois números.
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

void aplicar(const protocolo::Comando &comando);

// Lê o que a Pi manda. Os comandos chegam raramente (o $H a cada 60 s); ler a cada poucos ms
// basta e quase não soma atraso ao $H.
void tarefa_leitura(void *) {
  for (;;) {
    int caractere;
    while ((caractere = Serial.read()) >= 0) {
      if (leitor.receber(static_cast<char>(caractere))) {
        aplicar(protocolo::interpretar(leitor.tipo(), leitor.campos()));
      }
    }
    vTaskDelay(pdMS_TO_TICKS(config::serial_periodo_leitura_ms));
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
  if (xTaskCreatePinnedToCore(tarefa_leitura, "serial_pi_leitura", config::serial_pilha_leitura,
                              nullptr, config::serial_prioridade_leitura, nullptr,
                              tskNO_AFFINITY) != pdPASS) {
    abort();
  }
}

void emitir_boot(protocolo::MotivoReset motivo) {
  ComTrava com_trava;
  emissor.emitir_boot(relogio.t(agora_ms()), versao_firmware, motivo);
}

void emitir_status(protocolo::Status status) {
  ComTrava com_trava;
  // Com a trava, o estado e o t saem da mesma leitura do relógio: nunca ESPERA com t.
  status.estado = relogio.acertado() ? protocolo::Estado::ativo : protocolo::Estado::espera;
  status.obd_hz_decimos = obd_hz_em_uso;
  emissor.emitir_status(relogio.t(agora_ms()), status);
}

void emitir_log(protocolo::Nivel nivel, const char *texto) {
  ComTrava com_trava;
  emissor.emitir_log(relogio.t(agora_ms()), nivel, texto);
}

void emitir_acelerometro(int32_t ax, int32_t ay, int32_t az) {
  ComTrava com_trava;
  emissor.emitir_acelerometro(relogio.t(agora_ms()), ax, ay, az);
}

void emitir_obd(const char *campo, int64_t valor_escalado, uint8_t casas) {
  ComTrava com_trava;
  emissor.emitir_obd(relogio.t(agora_ms()), campo, valor_escalado, casas);
}

void emitir_pids(uint32_t mapa00, uint32_t mapa20, uint32_t mapa40) {
  ComTrava com_trava;
  emissor.emitir_pids(relogio.t(agora_ms()), mapa00, mapa20, mapa40);
}

uint16_t obd_hz_decimos() {
  ComTrava com_trava;
  return obd_hz_em_uso;
}

bool ativo() {
  ComTrava com_trava;
  return relogio.acertado();
}

namespace {

void aplicar(const protocolo::Comando &comando) {
  using Tipo = protocolo::Comando::Tipo;
  switch (comando.tipo) {
    case Tipo::hora: {
      ComTrava com_trava;
      relogio.acertar(comando.unix_ms, agora_ms());
      // O $H recebido também vai para o SD, na ordem das linhas emitidas.
      protocolo::Linha linha('H');
      linha.campo_inteiro(comando.unix_ms);
      if (linha.fechar()) {
        gravacao_sd::enfileirar(linha.dados(), linha.tamanho());
      }
      break;
    }
    case Tipo::hora_invalida:
      emitir_log(protocolo::Nivel::aviso, "H invalido");
      break;
    case Tipo::taxa_obd: {
      ComTrava com_trava;
      obd_hz_em_uso = comando.obd_hz_decimos;
      break;
    }
    case Tipo::taxa_invalida:
      emitir_log(protocolo::Nivel::aviso, "C invalido");
      break;
    case Tipo::nenhum:
      break;
  }
}

}  // namespace

}  // namespace vguard::serial_pi
