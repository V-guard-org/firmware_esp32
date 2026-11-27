// NUCLEO 0: tarefas de serial RX/TX e SD. Os produtores só copiam linhas para buffers.
#include "interfaces.h"

// ==================== serial_pi ====================

#include <Arduino.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>

#include <cstdlib>

#include "config.h"

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
    {
      // A mesma trava serializa o fallback e o $H: a hora válida sempre prevalece.
      ComTrava com_trava;
      const uint64_t agora = agora_ms();
      if (relogio.ativar_autonomo(agora, config::espera_hora_ms)) {
        emissor.emitir_log(relogio.t(agora), protocolo::Nivel::aviso,
                          "Modo autonomo: t em ms desde boot");
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
                              config::saida_nucleo) != pdPASS) {
    abort();
  }
  if (xTaskCreatePinnedToCore(tarefa_leitura, "serial_pi_leitura", config::serial_pilha_leitura,
                              nullptr, config::serial_prioridade_leitura, nullptr,
                              config::saida_nucleo) != pdPASS) {
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
  status.estado = relogio.ativo() ? protocolo::Estado::ativo : protocolo::Estado::espera;
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
  return relogio.ativo();
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

// ==================== gravacao_sd ====================

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/stream_buffer.h>
#include <freertos/task.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "src/nucleo/arquivo_sd.h"
#include "config.h"

namespace vguard::gravacao_sd {

namespace {

using protocolo::EstadoSd;

// Onde o SD do Arduino monta o cartão no sistema de arquivos do ESP-IDF.
constexpr const char *ponto_de_montagem = "/sd";

SPIClass spi_sd(HSPI);
StreamBufferHandle_t fila = nullptr;
std::atomic<EstadoSd> estado_atual{EstadoSd::ausente};
std::atomic<uint8_t> livre_pct{0};
// Linhas que não couberam na fila desde o último aviso.
std::atomic<uint32_t> perdidas{0};

// O cartão para o Registro: junta na RAM o que ele escreve e grava de uma vez, a cada 1 s ou ao
// trocar de arquivo. Pelo stdio, e não pelo File do Arduino, para ver os erros da gravação e da
// confirmação (fsync), que o File engole.
class CartaoSd final : public arquivo_sd::Cartao {
 public:
  bool abrir_sem_hora() override {
    // Nome novo por boot autônomo. Nenhum arquivo anterior é sobrescrito.
    for (unsigned n = 1; n <= 99999; ++n) {
      char nome[16];
      char caminho[18];
      std::snprintf(nome, sizeof(nome), "SEM%05u.csv", n);
      std::snprintf(caminho, sizeof(caminho), "/%s", nome);
      if (!SD.exists(caminho)) {
        return abrir(nome);
      }
    }
    return false;
  }

  bool abrir(const char *nome) override {
    if (!gravar_pendente()) {
      return false;
    }
    fechar();
    char caminho[16 + arquivo_sd::tamanho_nome];
    std::snprintf(caminho, sizeof(caminho), "%s/%s", ponto_de_montagem, nome);
    arquivo_ = std::fopen(caminho, "a");
    if (arquivo_ == nullptr) {
      return false;
    }
    // Sem o buffer do stdio: o bloco já junta 1 s de linhas, e assim o erro aparece no fwrite.
    std::setvbuf(arquivo_, nullptr, _IONBF, 0);
    return true;
  }

  bool escrever(const char *texto, size_t tamanho) override {
    if (tamanho_ + tamanho > sizeof(pendente_) && !gravar_pendente()) {
      return false;
    }
    if (tamanho > sizeof(pendente_)) {
      return false;
    }
    std::memcpy(pendente_ + tamanho_, texto, tamanho);
    tamanho_ += tamanho;
    return true;
  }

  // Grava o que juntou e confirma no cartão. Num corte de energia, perde-se só o que ainda não
  // passou por aqui: no máximo 1 s.
  bool gravar_pendente() {
    if (tamanho_ == 0) {
      return true;
    }
    const size_t quantidade = tamanho_;
    tamanho_ = 0;
    return arquivo_ != nullptr && std::fwrite(pendente_, 1, quantidade, arquivo_) == quantidade &&
           fsync(fileno(arquivo_)) == 0;
  }

  void fechar() {
    if (arquivo_ != nullptr) {
      std::fclose(arquivo_);
      arquivo_ = nullptr;
    }
  }

 private:
  FILE *arquivo_ = nullptr;
  char pendente_[config::sd_tamanho_bloco];
  size_t tamanho_ = 0;
};

CartaoSd cartao;
arquivo_sd::Registro registro(cartao, versao_firmware);

void medir_livre() {
  const uint64_t total = SD.totalBytes();
  const uint64_t usado = SD.usedBytes();
  livre_pct = total > 0 && usado <= total ? static_cast<uint8_t>((total - usado) * 100 / total) : 0;
}

void avisar_perdidas() {
  const uint32_t quantidade = perdidas.exchange(0);
  if (quantidade == 0) {
    return;
  }
  char texto[protocolo::tamanho_maximo_texto_log + 1];
  std::snprintf(texto, sizeof(texto), "SD perdeu %lu linhas",
                static_cast<unsigned long>(quantidade));
  serial_pi::emitir_log(protocolo::Nivel::aviso, texto);
}

void tarefa(void *) {
  TickType_t despertar = xTaskGetTickCount();
  TickType_t ultima_medida = despertar;
  for (;;) {
    vTaskDelayUntil(&despertar, pdMS_TO_TICKS(config::sd_periodo_gravacao_ms));
    char bloco[256];
    size_t quantidade;
    while ((quantidade = xStreamBufferReceive(fila, bloco, sizeof(bloco), 0)) > 0) {
      registro.receber(bloco, quantidade);
    }
    if (registro.falhou() || !cartao.gravar_pendente()) {
      // FALHA até o próximo boot: ninguém mais põe linhas na fila, e a tarefa termina.
      estado_atual = EstadoSd::falha;
      cartao.fechar();
      vTaskDelete(nullptr);
    }
    avisar_perdidas();
    const TickType_t agora = xTaskGetTickCount();
    if (agora - ultima_medida >= pdMS_TO_TICKS(config::sd_periodo_livre_ms)) {
      ultima_medida = agora;
      medir_livre();
    }
  }
}

}  // namespace

void preparar() {
  spi_sd.begin(pinos::sd_sck, pinos::sd_miso, pinos::sd_mosi, pinos::sd_cs);
  if (!SD.begin(pinos::sd_cs, spi_sd, config::sd_spi_hz, ponto_de_montagem) ||
      SD.cardType() == CARD_NONE) {
    estado_atual = EstadoSd::ausente;
    return;
  }
  // Sem memória já no setup(), seguir sem o SD não serve: reinicia (PANE no $B).
  fila = xStreamBufferCreate(config::sd_tamanho_fila, 1);
  if (fila == nullptr) {
    abort();
  }
  medir_livre();
  estado_atual = EstadoSd::ok;
}

void iniciar() {
  if (estado_atual != EstadoSd::ok) {
    return;
  }
  if (xTaskCreatePinnedToCore(tarefa, "gravacao_sd", config::sd_pilha, nullptr,
                              config::sd_prioridade, nullptr, config::sd_nucleo) != pdPASS) {
    abort();
  }
}

void enfileirar(const char *linha, size_t tamanho) {
  if (fila == nullptr || estado_atual != EstadoSd::ok) {
    return;
  }
  // Com um só escritor (a trava da serial), o espaço visto aqui não diminui antes do envio, e a
  // linha entra inteira ou não entra.
  if (xStreamBufferSpacesAvailable(fila) < tamanho ||
      xStreamBufferSend(fila, linha, tamanho, 0) != tamanho) {
    ++perdidas;
  }
}

EstadoSd estado() { return estado_atual; }

uint8_t livre() { return livre_pct; }

}  // namespace vguard::gravacao_sd
