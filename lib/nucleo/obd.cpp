#include "obd.h"

#include <cmath>

namespace vguard::obd {

namespace {

// Copiadas do obd_checks.h do Nicolas: conferidas na compilação, no ESP32 e no notebook.
namespace verificacoes {
static_assert(filtro_padrao(0x7FF) == 0x07FF0000UL, "Máscara do SID sem filtrar dados");
static_assert(filtro_padrao(0x7E8) == 0x07E80000UL, "Filtro do SID da central");
static_assert(filtro_padrao(0x7E8) != 0x000007E8UL, "Recusar o formato errado");
static_assert(!tempo_esgotado(100, 150, 1200), "Não expirar com horário anterior ao envio");
static_assert(!tempo_esgotado(1349, 150, 1200), "Não expirar antes do limite");
static_assert(tempo_esgotado(1350, 150, 1200), "Expirar no limite");
static_assert(tempo_esgotado(20, 0xFFFFFFF0UL, 30), "O tempo limite atravessa a volta do contador");
constexpr uint8_t rpm[] = {4, 0x41, 0x0C, 0x0D, 0xB0, 0, 0, 0};
constexpr uint8_t negativa[] = {3, 0x7F, 1, 0x11};
constexpr uint8_t multiquadro[] = {0x10, 0x0A, 0x41, 0x0C, 0, 0, 0, 0};
constexpr bool quadro(uint32_t id, uint8_t dlc, const uint8_t *d, uint8_t pid) {
  uint8_t bytes = 0;
  return resposta_positiva(id, dlc, d, 0x7E8, pid, bytes);
}
constexpr float valor(uint8_t pid, uint8_t a, uint8_t b, uint8_t n) {
  const uint8_t dados[] = {a, b};
  float v = 0;
  return decodificar(pid, dados, n, v) ? v : -99999;
}
constexpr bool perto(float a, float b) { return a > b - 0.001f && a < b + 0.001f; }
static_assert(quadro(0x7E8, 8, rpm, 0x0C), "Resposta de RPM válida");
static_assert(!quadro(0x7E9, 8, rpm, 0x0C), "Recusar outra central");
static_assert(!quadro(0x800007E8UL, 8, rpm, 0x0C), "Recusar quadro estendido");
static_assert(!quadro(0x400007E8UL, 8, rpm, 0x0C), "Recusar RTR");
static_assert(!quadro(0x7E8, 4, rpm, 0x0C), "Recusar quadro truncado");
static_assert(!quadro(0x7E8, 9, rpm, 0x0C), "Recusar DLC inválido");
static_assert(!quadro(0x7E8, 8, rpm, 0x0D), "Recusar PID diferente");
static_assert(!quadro(0x7E8, 4, negativa, 1), "Recusar resposta negativa");
static_assert(!quadro(0x7E8, 8, multiquadro, 0x0C), "Recusar resposta segmentada");
static_assert(valor(0x0C, 0x0D, 0xB0, 2) == 876, "RPM lido no Voyage");
static_assert(valor(0x0C, 0x1A, 0xF8, 2) == 1726, "Exemplo de RPM");
static_assert(valor(0x0D, 0, 0, 1) == 0, "Zero é leitura válida");
static_assert(valor(0x0D, 60, 0, 1) == 60, "Velocidade direta");
static_assert(valor(0x05, 87, 0, 1) == 47, "Temperatura lida no Voyage");
static_assert(valor(0x05, 0, 0, 1) == -40, "Temperatura negativa");
static_assert(perto(valor(0x42, 0x35, 0xE8, 2), 13.8f), "Tensão da central");
static_assert(valor(0x04, 255, 0, 1) == 100, "Percentual");
static_assert(valor(0x06, 128, 0, 1) == 0, "Correção de combustível neutra");
static_assert(valor(0x0C, 1, 0, 1) == -99999, "Recusar RPM incompleto");
static_assert(valor(0x01, 1, 0, 1) == -99999, "PID codificado fica de fora");
static_assert(suportado(0x80000001UL, 1), "Primeiro bit do mapa");
static_assert(suportado(0x80000001UL, 32), "Bit do próximo mapa");
static_assert(!suportado(0x80000001UL, 2), "Bit ausente");
static_assert(!suportado(0xFFFFFFFFUL, 0), "Deslocamento zero inválido");
static_assert(!suportado(0xFFFFFFFFUL, 33), "Deslocamento acima do mapa");
constexpr uint8_t um_mapa[] = {0x80, 0, 0, 1};
static_assert(mapa(um_mapa) == 0x80000001UL, "Mapa do mais significativo para o menos");
}  // namespace verificacoes

// Os 8 PIDs de interesse estão nos três mapas lidos (00, 20 e 40).
constexpr bool campos_nos_mapas() {
  for (const Campo &campo : campos) {
    if (campo.pid >= 0x60 || pid_de_mapa(campo.pid) || bytes_esperados(campo.pid) == 0) {
      return false;
    }
  }
  return true;
}
static_assert(campos_nos_mapas(), "Cada PID de interesse está num dos mapas e tem tamanho");

// A taxa padrão, se vier zero: 12,5 pedidos por segundo.
constexpr uint16_t obd_hz_padrao_decimos = 125;

}  // namespace

int64_t valor_escalado(float valor, uint8_t casas) {
  double escala = 1;
  for (uint8_t i = 0; i < casas; ++i) {
    escala *= 10;
  }
  return std::llround(static_cast<double>(valor) * escala);
}

void Maquina::comecar(uint32_t agora) {
  static_cast<void>(agora);
  estado_ = protocolo::EstadoObd::escutando;
  viu_trafego_ = false;
  aguardando_ = false;
}

void Maquina::receber(const Quadro &quadro, uint32_t agora) {
  if (estado_ == protocolo::EstadoObd::parado) {
    return;
  }
  viu_trafego_ = true;
  ultimo_trafego_ = agora;
  if (!aguardando_) {
    return;
  }
  // A recusa, o "aguarde" e o que não é a resposta do pedido esperam o tempo limite (as regras
  // da recusa vêm no #7).
  uint8_t bytes = 0;
  if (!resposta_positiva(quadro.id, quadro.dlc, quadro.dados, id_resposta, pendente_, bytes)) {
    return;
  }
  if (estado_ == protocolo::EstadoObd::descobrindo) {
    if (bytes != 4) {
      return;
    }
    aguardando_ = false;
    responder_mapa(quadro.dados + 3);
    return;
  }
  if (bytes != bytes_esperados(pendente_)) {
    return;
  }
  aguardando_ = false;
  float valor = 0;
  for (const Campo &campo : campos) {
    if (campo.pid == pendente_ && decodificar(pendente_, quadro.dados + 3, bytes, valor)) {
      saida_.emitir_obd(campo.nome, valor_escalado(valor, campo.casas), campo.casas);
    }
  }
}

void Maquina::tique(uint32_t agora, uint16_t obd_hz_decimos) {
  switch (estado_) {
    case protocolo::EstadoObd::escutando:
      if (viu_trafego_ && uint32_t(agora - ultimo_trafego_) <= trafego_recente_ms) {
        comecar_descoberta(agora);
        break;
      }
      return;
    case protocolo::EstadoObd::descobrindo:
    case protocolo::EstadoObd::lendo:
      break;
    default:
      return;
  }
  if (aguardando_) {
    if (!tempo_esgotado(agora, enviado_em_, tempo_limite_ms)) {
      return;
    }
    // Sem resposta: na leitura, o rodízio já está no próximo PID; na descoberta, o mapa da vez
    // não mudou e é pedido de novo.
    aguardando_ = false;
  }
  if (!tempo_esgotado(agora, proximo_ms_, 0)) {
    return;
  }
  if (estado_ == protocolo::EstadoObd::descobrindo) {
    pedir(mapa_atual_, agora, obd_hz_decimos);
  } else if (quantidade_ativos_ > 0) {
    const uint8_t campo = ativos_[proximo_ativo_];
    proximo_ativo_ = static_cast<uint8_t>((proximo_ativo_ + 1) % quantidade_ativos_);
    pedir(campos[campo].pid, agora, obd_hz_decimos);
  }
}

void Maquina::comecar_descoberta(uint32_t agora) {
  saida_.configurar_leitura();
  estado_ = protocolo::EstadoObd::descobrindo;
  aguardando_ = false;
  mapa_atual_ = 0;
  for (uint32_t &m : mapas_) {
    m = 0;
  }
  // O mapa 00 sai já neste tique.
  proximo_ms_ = agora;
  fracao_us_ = 0;
}

void Maquina::pedir(uint8_t pid, uint32_t agora, uint16_t obd_hz_decimos) {
  const uint8_t dados[8] = {2, 1, pid, 0, 0, 0, 0, 0};
  const bool primeiro_mapa = estado_ == protocolo::EstadoObd::descobrindo && pid == 0;
  saida_.enviar(primeiro_mapa ? id_todos : id_central, dados);
  aguardando_ = true;
  pendente_ = pid;
  enviado_em_ = agora;
  marcar_proximo(agora, obd_hz_decimos);
}

void Maquina::marcar_proximo(uint32_t agora, uint16_t obd_hz_decimos) {
  const uint32_t decimos = obd_hz_decimos > 0 ? obd_hz_decimos : obd_hz_padrao_decimos;
  // Em µs: 10^7 / décimos. A 12,5 por segundo, 80 000 µs.
  const uint32_t intervalo_us = 10000000UL / decimos;
  // Atrasado pela central (e não só pela leitura a cada 10 ms): a agenda recomeça do envio, em
  // vez de mandar os pedidos atrasados em rajada.
  if (int32_t(agora - proximo_ms_) >= int32_t(folga_da_agenda_ms)) {
    proximo_ms_ = agora;
    fracao_us_ = 0;
  }
  const uint32_t total_us = fracao_us_ + intervalo_us;
  proximo_ms_ += total_us / 1000;
  fracao_us_ = total_us % 1000;
}

void Maquina::responder_mapa(const uint8_t *dados_do_mapa) {
  const uint32_t valor = mapa(dados_do_mapa);
  mapas_[mapa_atual_ / 0x20] = valor;
  // O bit 32 anuncia o mapa seguinte. Ele sai pela agenda, no próximo tique na hora.
  if (mapa_atual_ < 0x40 && suportado(valor, 32)) {
    mapa_atual_ = static_cast<uint8_t>(mapa_atual_ + 0x20);
  } else {
    terminar_descoberta();
  }
}

void Maquina::terminar_descoberta() {
  quantidade_ativos_ = 0;
  for (uint8_t i = 0; i < quantidade_campos; ++i) {
    const uint8_t pid = campos[i].pid;
    const uint8_t base = static_cast<uint8_t>(pid / 0x20);
    if (suportado(mapas_[base], static_cast<uint8_t>(pid - base * 0x20))) {
      ativos_[quantidade_ativos_++] = i;
    }
  }
  proximo_ativo_ = 0;
  estado_ = protocolo::EstadoObd::lendo;
  saida_.emitir_pids(mapas_[0], mapas_[1], mapas_[2]);
}

}  // namespace vguard::obd
