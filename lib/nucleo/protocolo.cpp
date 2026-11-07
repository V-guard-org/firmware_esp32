#include "protocolo.h"

namespace vguard::protocolo {

namespace {

constexpr char digitos_hexa[] = "0123456789ABCDEF";

// O *hh que o fechar() acrescenta. A linha reserva esse espaço desde o começo.
constexpr size_t tamanho_checksum = 3;

// Até 10^18, que ainda cabe num uint64_t.
constexpr uint8_t maximo_casas = 18;

}  // namespace

uint8_t checksum(const char *texto, size_t tamanho) {
  uint8_t resultado = 0;
  for (size_t i = 0; i < tamanho; ++i) {
    resultado ^= static_cast<uint8_t>(texto[i]);
  }
  return resultado;
}

Linha::Linha(char tipo) {
  acrescentar('$');
  acrescentar(tipo);
}

void Linha::acrescentar(char caractere) {
  if (fechada_ || !valida_) {
    return;
  }
  if (tamanho_ >= tamanho_maximo - tamanho_checksum) {
    valida_ = false;
    return;
  }
  buffer_[tamanho_++] = caractere;
}

void Linha::virgula() { acrescentar(','); }

void Linha::acrescentar_digitos(uint64_t valor, uint8_t minimo) {
  // Os dígitos saem do menos significativo para o mais; um uint64_t tem até 20.
  char invertidos[20];
  uint8_t quantidade = 0;
  do {
    invertidos[quantidade++] = static_cast<char>('0' + valor % 10);
    valor /= 10;
  } while (valor > 0);
  while (quantidade < minimo && quantidade < sizeof(invertidos)) {
    invertidos[quantidade++] = '0';
  }
  while (quantidade > 0) {
    acrescentar(invertidos[--quantidade]);
  }
}

Linha &Linha::campo_texto(const char *texto) {
  virgula();
  if (texto == nullptr) {
    valida_ = false;
    return *this;
  }
  for (const char *c = texto; *c != '\0'; ++c) {
    if (*c == '$' || *c == '*' || *c == '\n' || *c == '\r') {
      valida_ = false;
      return *this;
    }
    acrescentar(*c);
  }
  return *this;
}

Linha &Linha::campo_inteiro(uint64_t valor) {
  virgula();
  acrescentar_digitos(valor, 1);
  return *this;
}

Linha &Linha::campo_decimal(int64_t valor_escalado, uint8_t casas) {
  virgula();
  if (casas > maximo_casas) {
    valida_ = false;
    return *this;
  }
  uint64_t escala = 1;
  for (uint8_t i = 0; i < casas; ++i) {
    escala *= 10;
  }
  // Pela subtração em uint64_t, o menor int64_t também vira positivo sem estourar.
  const uint64_t modulo = valor_escalado < 0 ? 0 - static_cast<uint64_t>(valor_escalado)
                                             : static_cast<uint64_t>(valor_escalado);
  if (valor_escalado < 0) {
    acrescentar('-');
  }
  acrescentar_digitos(modulo / escala, 1);
  if (casas > 0) {
    acrescentar('.');
    acrescentar_digitos(modulo % escala, casas);
  }
  return *this;
}

bool Linha::fechar() {
  if (fechada_) {
    return valida_;
  }
  fechada_ = true;
  if (!valida_) {
    return false;
  }
  // O checksum vai do caractere depois do $ até o último antes do *.
  const uint8_t soma = checksum(buffer_ + 1, tamanho_ - 1);
  buffer_[tamanho_++] = '*';
  buffer_[tamanho_++] = digitos_hexa[soma >> 4];
  buffer_[tamanho_++] = digitos_hexa[soma & 0x0F];
  buffer_[tamanho_++] = '\n';
  buffer_[tamanho_] = '\0';
  return true;
}

const char *nome(MotivoReset motivo) {
  switch (motivo) {
    case MotivoReset::ligou: return "LIGOU";
    case MotivoReset::software: return "SOFTWARE";
    case MotivoReset::pane: return "PANE";
    case MotivoReset::watchdog: return "WATCHDOG";
    case MotivoReset::queda_tensao: return "QUEDA_TENSAO";
    case MotivoReset::outro: return "OUTRO";
  }
  return "OUTRO";
}

const char *nome(Estado estado) {
  switch (estado) {
    case Estado::espera: return "ESPERA";
    case Estado::ativo: return "ATIVO";
  }
  return "?";
}

const char *nome(EstadoObd obd) {
  switch (obd) {
    case EstadoObd::parado: return "PARADO";
    case EstadoObd::escutando: return "ESCUTANDO";
    case EstadoObd::descobrindo: return "DESCOBRINDO";
    case EstadoObd::lendo: return "LENDO";
    case EstadoObd::sem_resposta: return "SEM_RESPOSTA";
    case EstadoObd::falha: return "FALHA";
  }
  return "?";
}

const char *nome(EstadoImu imu) {
  switch (imu) {
    case EstadoImu::ok: return "OK";
    case EstadoImu::falha: return "FALHA";
  }
  return "?";
}

const char *nome(EstadoSd sd) {
  switch (sd) {
    case EstadoSd::ok: return "OK";
    case EstadoSd::ausente: return "AUSENTE";
    case EstadoSd::cheio: return "CHEIO";
    case EstadoSd::falha: return "FALHA";
  }
  return "?";
}

Linha Emissor::comecar(char tipo, uint64_t t) {
  Linha linha(tipo);
  linha.campo_inteiro(proxima_seq_++).campo_inteiro(t);
  return linha;
}

void Emissor::emitir(Linha &linha) {
  // A linha inválida (passou de 120 caracteres) também conta: ela não sai, como a que não
  // coube.
  if (!linha.fechar() || !destino_.escrever(linha.dados(), linha.tamanho())) {
    ++descartadas_;
  }
}

void Emissor::emitir_boot(uint64_t t, const char *firmware, MotivoReset motivo) {
  Linha linha = comecar('B', t);
  linha.campo_texto(firmware).campo_texto(nome(motivo));
  emitir(linha);
}

void Emissor::emitir_status(uint64_t t, const Status &status) {
  Linha linha = comecar('S', t);
  linha.campo_inteiro(status.uptime_s)
      .campo_texto(nome(status.estado))
      .campo_texto(nome(status.obd))
      .campo_texto(nome(status.imu))
      .campo_texto(nome(status.sd))
      .campo_inteiro(status.sd_livre);
  // Com até 1 casa decimal: 12.5, mas 15 e não 15.0.
  if (status.obd_hz_decimos % 10 == 0) {
    linha.campo_inteiro(status.obd_hz_decimos / 10);
  } else {
    linha.campo_decimal(status.obd_hz_decimos, 1);
  }
  linha.campo_inteiro(descartadas_);
  emitir(linha);
}

}  // namespace vguard::protocolo
