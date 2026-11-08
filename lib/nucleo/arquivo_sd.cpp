#include "arquivo_sd.h"

#include <cstring>

namespace vguard::arquivo_sd {

namespace {

constexpr uint64_t ms_por_dia = 24 * ms_por_hora;

// Dois dígitos decimais, com zero à esquerda.
void dois_digitos(char *destino, uint32_t valor) {
  destino[0] = static_cast<char>('0' + valor / 10 % 10);
  destino[1] = static_cast<char>('0' + valor % 10);
}

// O t de uma linha sem a moldura ("A,57,1793980812457,..."): o 3º campo, ou o 2º no $H, que não
// tem seq. Devolve 0 se o campo não é um número.
uint64_t t_da_linha(const char *texto, size_t tamanho) {
  const size_t campo = texto[0] == 'H' ? 1 : 2;
  size_t i = 0;
  for (size_t virgulas = 0; i < tamanho && virgulas < campo; ++i) {
    virgulas += texto[i] == ',';
  }
  uint64_t t = 0;
  bool tem_digito = false;
  for (; i < tamanho && texto[i] >= '0' && texto[i] <= '9'; ++i) {
    t = t * 10 + static_cast<uint64_t>(texto[i] - '0');
    tem_digito = true;
  }
  return tem_digito ? t : 0;
}

}  // namespace

void nome_do_arquivo(uint64_t t, char (&nome)[tamanho_nome]) {
  // A data civil (UTC) a partir dos dias desde 01/01/1970, pelo algoritmo civil_from_days de
  // Howard Hinnant (http://howardhinnant.github.io/date_algorithms.html), sem datas negativas.
  const uint64_t z = t / ms_por_dia + 719468;
  const uint64_t era = z / 146097;
  const uint64_t doe = z - era * 146097;
  const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const uint64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const uint64_t mp = (5 * doy + 2) / 153;
  const uint32_t dia = static_cast<uint32_t>(doy - (153 * mp + 2) / 5 + 1);
  const uint32_t mes = static_cast<uint32_t>(mp < 10 ? mp + 3 : mp - 9);
  const uint32_t ano = static_cast<uint32_t>(yoe + era * 400 + (mes <= 2 ? 1 : 0));
  const uint32_t hora = static_cast<uint32_t>(t % ms_por_dia / ms_por_hora);

  dois_digitos(nome, ano / 100);
  dois_digitos(nome + 2, ano % 100);
  dois_digitos(nome + 4, mes);
  dois_digitos(nome + 6, dia);
  nome[8] = '-';
  dois_digitos(nome + 9, hora);
  std::memcpy(nome + 11, ".csv", 5);
}

void Registro::receber(const char *bytes, size_t tamanho) {
  for (size_t i = 0; i < tamanho; ++i) {
    const char caractere = bytes[i];
    if (caractere == '\n') {
      if (!descartando_) {
        linha_completa();
      }
      tamanho_linha_ = 0;
      descartando_ = false;
    } else if (descartando_) {
      continue;
    } else if (tamanho_linha_ < protocolo::tamanho_maximo) {
      linha_[tamanho_linha_++] = caractere;
    } else {
      descartando_ = true;
    }
  }
}

void Registro::linha_completa() {
  if (falhou_) {
    return;
  }
  // "$<tipo>,<campos>*hh": o texto vai do caractere depois do $ até o anterior ao *.
  size_t tamanho = tamanho_linha_;
  if (tamanho > 0 && linha_[tamanho - 1] == '\r') {
    --tamanho;
  }
  if (tamanho < 5 || linha_[0] != '$' || linha_[tamanho - 3] != '*') {
    return;
  }
  char *texto = linha_ + 1;
  size_t tamanho_texto = tamanho - 4;
  const char tipo = texto[0];
  // Com o \n, no lugar do *.
  texto[tamanho_texto] = '\n';

  bool gravar_boot = false;
  if (!tem_hora_) {
    if (tipo == 'B') {
      std::memcpy(boot_, texto, tamanho_texto + 1);
      tamanho_boot_ = tamanho_texto + 1;
    }
    if (tipo != 'H') {
      return;
    }
    tem_hora_ = true;
    gravar_boot = tamanho_boot_ > 0;
  }

  const uint64_t t = t_da_linha(texto, tamanho_texto);
  if (t != 0 && (!tem_arquivo_ || t / ms_por_hora != hora_) && !abrir_arquivo(t)) {
    return;
  }
  if (!tem_arquivo_) {
    return;
  }
  if (gravar_boot && !gravar(boot_, tamanho_boot_)) {
    return;
  }
  gravar(texto, tamanho_texto + 1);
}

bool Registro::abrir_arquivo(uint64_t t) {
  char nome[tamanho_nome];
  nome_do_arquivo(t, nome);
  if (!cartao_.abrir(nome)) {
    falhou_ = true;
    return false;
  }
  tem_arquivo_ = true;
  hora_ = t / ms_por_hora;
  constexpr char inicio[] = "# V-GUARD ";
  constexpr char separador[] = " | ";
  return gravar(inicio, sizeof(inicio) - 1) && gravar(versao_, std::strlen(versao_)) &&
         gravar(separador, sizeof(separador) - 1) && gravar(colunas, std::strlen(colunas)) &&
         gravar("\n", 1);
}

bool Registro::gravar(const char *texto, size_t tamanho) {
  if (!cartao_.escrever(texto, tamanho)) {
    falhou_ = true;
    return false;
  }
  return true;
}

}  // namespace vguard::arquivo_sd
