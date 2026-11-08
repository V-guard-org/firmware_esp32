#include "gravacao_sd.h"

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

#include "arquivo_sd.h"
#include "config.h"
#include "serial_pi.h"

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
