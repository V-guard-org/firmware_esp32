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

Linha &Linha::campo_texto(const char *texto) { return campo_texto(texto, tamanho_maximo); }

Linha &Linha::campo_texto(const char *texto, size_t maximo) {
  virgula();
  if (texto == nullptr) {
    valida_ = false;
    return *this;
  }
  for (const char *c = texto; *c != '\0' && static_cast<size_t>(c - texto) < maximo; ++c) {
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

const char *nome(Nivel nivel) {
  switch (nivel) {
    case Nivel::aviso: return "AVISO";
    case Nivel::erro: return "ERRO";
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

void Emissor::emitir_log(uint64_t t, Nivel nivel, const char *texto) {
  Linha linha = comecar('L', t);
  linha.campo_texto(nome(nivel)).campo_texto(texto, tamanho_maximo_texto_log);
  emitir(linha);
}

namespace {

bool hexa_maiusculo(char caractere, uint8_t &valor) {
  if (caractere >= '0' && caractere <= '9') {
    valor = static_cast<uint8_t>(caractere - '0');
    return true;
  }
  if (caractere >= 'A' && caractere <= 'F') {
    valor = static_cast<uint8_t>(caractere - 'A' + 10);
    return true;
  }
  return false;
}

bool digito(char caractere) { return caractere >= '0' && caractere <= '9'; }

// Lê um inteiro sem sinal só com dígitos, de 1 a 19 (até 10^19 − 1, que cabe em 64 bits).
bool ler_inteiro(const char *texto, uint64_t &valor) {
  constexpr size_t maximo_digitos = 19;
  valor = 0;
  size_t quantidade = 0;
  for (const char *c = texto; *c != '\0'; ++c) {
    if (!digito(*c) || ++quantidade > maximo_digitos) {
      return false;
    }
    valor = valor * 10 + static_cast<uint64_t>(*c - '0');
  }
  return quantidade > 0;
}

// $H,<unix_ms>
Comando interpretar_hora(const char *campos) {
  Comando comando;
  uint64_t unix_ms = 0;
  if (ler_inteiro(campos, unix_ms) && unix_ms >= hora_minima_ms) {
    comando.tipo = Comando::Tipo::hora;
    comando.unix_ms = unix_ms;
  } else {
    comando.tipo = Comando::Tipo::hora_invalida;
  }
  return comando;
}

// $C,obd_hz,<n>: n com 1 ou 2 dígitos inteiros e, se tiver ponto, exatamente 1 casa.
Comando interpretar_configuracao(const char *campos) {
  constexpr char parametro[] = "obd_hz,";
  constexpr size_t tamanho_parametro = sizeof(parametro) - 1;
  constexpr uint16_t minimo_decimos = 10;   // 1 pedido por segundo
  constexpr uint16_t maximo_decimos = 200;  // 20 pedidos por segundo

  Comando comando;
  comando.tipo = Comando::Tipo::taxa_invalida;
  for (size_t i = 0; i < tamanho_parametro; ++i) {
    if (campos[i] != parametro[i]) {
      return comando;
    }
  }
  const char *c = campos + tamanho_parametro;
  uint16_t decimos = 0;
  size_t inteiros = 0;
  for (; digito(*c); ++c) {
    if (++inteiros > 2) {
      return comando;
    }
    decimos = static_cast<uint16_t>(decimos * 10 + (*c - '0'));
  }
  if (inteiros == 0) {
    return comando;
  }
  decimos = static_cast<uint16_t>(decimos * 10);
  if (*c == '.') {
    if (!digito(c[1]) || c[2] != '\0') {
      return comando;
    }
    decimos = static_cast<uint16_t>(decimos + (c[1] - '0'));
  } else if (*c != '\0') {
    return comando;
  }
  if (decimos < minimo_decimos || decimos > maximo_decimos) {
    return comando;
  }
  comando.tipo = Comando::Tipo::taxa_obd;
  comando.obd_hz_decimos = decimos;
  return comando;
}

}  // namespace

bool Leitor::receber(char caractere) {
  if (caractere == '\n') {
    const bool aceita = !descartando_ && aceitar();
    tamanho_ = 0;
    descartando_ = false;
    return aceita;
  }
  if (descartando_) {
    return false;
  }
  // Cabem os 120 caracteres e um \r; o \0 marcaria o fim do texto antes da hora.
  if (caractere == '\0' || tamanho_ >= tamanho_maximo + 1) {
    descartando_ = true;
    return false;
  }
  buffer_[tamanho_++] = caractere;
  return false;
}

bool Leitor::aceitar() {
  size_t tamanho = tamanho_;
  if (tamanho > 0 && buffer_[tamanho - 1] == '\r') {
    --tamanho;
  }
  // O menor é "$X*hh".
  if (tamanho < 5 || tamanho > tamanho_maximo || buffer_[0] != '$') {
    return false;
  }
  const size_t asterisco = tamanho - 3;
  uint8_t alto = 0;
  uint8_t baixo = 0;
  if (buffer_[asterisco] != '*' || !hexa_maiusculo(buffer_[asterisco + 1], alto) ||
      !hexa_maiusculo(buffer_[asterisco + 2], baixo)) {
    return false;
  }
  if (checksum(buffer_ + 1, asterisco - 1) != static_cast<uint8_t>((alto << 4) | baixo)) {
    return false;
  }
  // Depois do tipo, ou vêm os campos, depois de uma vírgula, ou já vem o *.
  if (asterisco == 2) {
    inicio_campos_ = 2;
  } else if (buffer_[2] == ',') {
    inicio_campos_ = 3;
  } else {
    return false;
  }
  if (buffer_[1] == '*' || buffer_[1] == ',') {
    return false;
  }
  buffer_[asterisco] = '\0';
  return true;
}

Comando interpretar(char tipo, const char *campos) {
  switch (tipo) {
    case 'H': return interpretar_hora(campos);
    case 'C': return interpretar_configuracao(campos);
    default: return Comando{};
  }
}

}  // namespace vguard::protocolo
