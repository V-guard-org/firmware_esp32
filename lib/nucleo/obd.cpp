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
  // A primeira descoberta não espera os 15 s: sai com o primeiro tráfego.
  parou_ = false;
  avisou_parada_ = false;
}

bool Maquina::pedindo() const {
  return estado_ == protocolo::EstadoObd::descobrindo || estado_ == protocolo::EstadoObd::lendo;
}

bool Maquina::trafego_recente(uint32_t agora) const {
  return viu_trafego_ && uint32_t(agora - ultimo_trafego_) <= trafego_recente_ms;
}

void Maquina::receber(const Quadro &quadro, uint32_t agora) {
  if (estado_ == protocolo::EstadoObd::parado || estado_ == protocolo::EstadoObd::falha) {
    return;
  }
  viu_trafego_ = true;
  ultimo_trafego_ = agora;
  if (!aguardando_) {
    return;
  }
  // A recusa conta como falha. O "aguarde" e o que não é a resposta do pedido esperam o tempo
  // limite.
  uint8_t codigo = 0;
  if (resposta_negativa(quadro.id, quadro.dlc, quadro.dados, id_resposta, codigo)) {
    if (codigo != codigo_aguarde) {
      falhar(Falha::recusa, agora);
    }
    return;
  }
  uint8_t bytes = 0;
  if (!resposta_positiva(quadro.id, quadro.dlc, quadro.dados, id_resposta, pendente_, bytes)) {
    return;
  }
  if (estado_ == protocolo::EstadoObd::descobrindo) {
    if (bytes != 4) {
      return;
    }
    aguardando_ = false;
    falhas_seguidas_ = 0;
    responder_mapa(quadro.dados + 3);
    return;
  }
  if (bytes != bytes_esperados(pendente_)) {
    return;
  }
  aguardando_ = false;
  falhas_seguidas_ = 0;
  falhas_[campo_pendente_] = 0;
  const Campo &campo = campos[campo_pendente_];
  float valor = 0;
  if (decodificar(pendente_, quadro.dados + 3, bytes, valor)) {
    saida_.emitir_obd(campo.nome, valor_escalado(valor, campo.casas), campo.casas);
  }
}

void Maquina::erro_no_can(uint32_t agora) {
  if (pedindo()) {
    parar(Motivo::erro_no_can, agora);
  }
}

void Maquina::tique(uint32_t agora, uint16_t obd_hz_decimos) {
  switch (estado_) {
    case protocolo::EstadoObd::escutando:
    case protocolo::EstadoObd::sem_resposta: {
      const bool trafego = trafego_recente(agora);
      estado_ = trafego ? protocolo::EstadoObd::sem_resposta : protocolo::EstadoObd::escutando;
      const bool esperou = !parou_ || uint32_t(agora - parou_em_) >= intervalo_tentativas_ms;
      if (!trafego || !esperou) {
        return;
      }
      comecar_descoberta(agora);
      if (!pedindo()) {
        return;
      }
      break;
    }
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
    falhar(Falha::sem_resposta, agora);
    if (!pedindo()) {
      return;
    }
  }
  if (!tempo_esgotado(agora, proximo_ms_, 0)) {
    return;
  }
  if (estado_ == protocolo::EstadoObd::descobrindo) {
    pedir(mapa_atual_, agora, obd_hz_decimos);
  } else if (!pedir_proximo_pid(agora, obd_hz_decimos)) {
    parar(Motivo::sem_pid_ativo, agora);
  }
}

void Maquina::comecar_descoberta(uint32_t agora) {
  // Sem os filtros e o modo normal, não dá para pedir: para, e o MCP2515 volta a só escutar.
  if (!saida_.configurar_leitura()) {
    parar(Motivo::erro_no_can, agora);
    return;
  }
  estado_ = protocolo::EstadoObd::descobrindo;
  aguardando_ = false;
  falhas_seguidas_ = 0;
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

bool Maquina::pedir_proximo_pid(uint32_t agora, uint16_t obd_hz_decimos) {
  // O próximo do rodízio que não está suspenso.
  for (uint8_t n = 0; n < quantidade_ativos_; ++n) {
    const uint8_t campo = ativos_[proximo_ativo_];
    proximo_ativo_ = static_cast<uint8_t>((proximo_ativo_ + 1) % quantidade_ativos_);
    if (!suspenso_[campo]) {
      campo_pendente_ = campo;
      pedir(campos[campo].pid, agora, obd_hz_decimos);
      return true;
    }
  }
  return false;
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
    falhas_[i] = 0;
    suspenso_[i] = false;
    const uint8_t pid = campos[i].pid;
    const uint8_t base = static_cast<uint8_t>(pid / 0x20);
    if (suportado(mapas_[base], static_cast<uint8_t>(pid - base * 0x20))) {
      ativos_[quantidade_ativos_++] = i;
    }
  }
  proximo_ativo_ = 0;
  falhas_seguidas_ = 0;
  estado_ = protocolo::EstadoObd::lendo;
  // A descoberta deu certo: encerra o episódio, e a próxima parada volta a avisar.
  avisou_parada_ = false;
  saida_.emitir_pids(mapas_[0], mapas_[1], mapas_[2]);
}

void Maquina::falhar(Falha falha, uint32_t agora) {
  aguardando_ = false;
  if (falhas_seguidas_ < falhas_seguidas_para_parar) {
    ++falhas_seguidas_;
  }
  if (estado_ == protocolo::EstadoObd::descobrindo) {
    if (mapa_atual_ == 0) {
      parar(Motivo::sem_resposta, agora);
    } else {
      // Os mapas 20 e 40 que faltam não impedem os PIDs já confirmados.
      terminar_descoberta();
    }
    return;
  }
  if (!suspenso_[campo_pendente_] && ++falhas_[campo_pendente_] >= falhas_para_suspender) {
    suspenso_[campo_pendente_] = true;
    avisar_pid_suspenso(pendente_, falha);
  }
  if (falha == Falha::sem_resposta && falhas_seguidas_ >= falhas_seguidas_para_parar) {
    parar(Motivo::sem_resposta, agora);
  }
}

void Maquina::parar(Motivo motivo, uint32_t agora) {
  aguardando_ = false;
  parou_ = true;
  parou_em_ = agora;
  // Só conta o tráfego visto depois da parada.
  viu_trafego_ = false;
  if (!avisou_parada_) {
    avisou_parada_ = true;
    switch (motivo) {
      case Motivo::sem_resposta: saida_.emitir_aviso("OBD sem resposta"); break;
      case Motivo::erro_no_can: saida_.emitir_aviso("OBD erro no CAN"); break;
      case Motivo::sem_pid_ativo: saida_.emitir_aviso("OBD sem PID ativo"); break;
    }
  }
  estado_ = saida_.voltar_a_escutar() ? protocolo::EstadoObd::escutando
                                      : protocolo::EstadoObd::falha;
}

void Maquina::avisar_pid_suspenso(uint8_t pid, Falha falha) {
  constexpr char hexa[] = "0123456789ABCDEF";
  // "PID 0F sem resposta" ou "PID 0F recusado".
  char texto[24] = "PID ";
  texto[4] = hexa[pid >> 4];
  texto[5] = hexa[pid & 0x0F];
  const char *resto = falha == Falha::sem_resposta ? " sem resposta" : " recusado";
  size_t i = 6;
  for (; *resto != '\0' && i + 1 < sizeof(texto); ++resto) {
    texto[i++] = *resto;
  }
  texto[i] = '\0';
  saida_.emitir_aviso(texto);
}

}  // namespace vguard::obd
