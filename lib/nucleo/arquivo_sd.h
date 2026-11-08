// O log no microSD (proposta §4.5): o nome do arquivo de cada hora, o cabeçalho e o registro que
// decide o que vai para o cartão. Só memória de tamanho fixo.
//
// O registro não chama o hardware: a parte do ESP32 passa a ele os bytes da fila (as linhas da
// serial, com a moldura) e executa o que ele pede pelo Cartao (abrir um arquivo, escrever).
#pragma once

#include <cstddef>
#include <cstdint>

#include "protocolo.h"

namespace vguard::arquivo_sd {

constexpr uint64_t ms_por_hora = 3600000ULL;

// "AAAAMMDD-HH.csv" e o \0.
constexpr size_t tamanho_nome = 16;

// O nome do arquivo da hora UTC do t (horário Unix em ms): 1794099493000 dá "20261108-00.csv".
void nome_do_arquivo(uint64_t t, char (&nome)[tamanho_nome]);

// As colunas de cada tipo de linha, depois da versão, no cabeçalho de cada arquivo. Os tipos
// são separados por " | ".
constexpr const char *colunas =
    "B,seq,t,firmware,motivo_reset"
    " | S,seq,t,uptime_s,estado,obd,imu,sd,sd_livre,obd_hz,descartadas"
    " | A,seq,t,ax,ay,az"
    " | O,seq,t,campo,valor"
    " | P,seq,t,mapa00,mapa20,mapa40"
    " | L,seq,t,nivel,texto"
    " | H,unix_ms";

// O cartão, visto pelo registro. A parte do ESP32 implementa sobre o SD do Arduino.
class Cartao {
 public:
  // Abre o arquivo para acrescentar no fim, e o cria se ele não existe. O anterior é fechado.
  // Devolve false se não deu.
  virtual bool abrir(const char *nome) = 0;
  // Acrescenta o texto no fim do arquivo aberto. Devolve false se não deu.
  virtual bool escrever(const char *texto, size_t tamanho) = 0;

 protected:
  ~Cartao() = default;
};

// Decide o que vai para o cartão (proposta §4.5). Recebe, na ordem, as linhas como saíram na
// serial (com a moldura) e os $H recebidos da Pi, também com a moldura:
// - antes do primeiro $H, nada é gravado. O $B fica guardado e é gravado logo que a hora chega,
//   antes do $H;
// - cada linha vai sem o $ e sem o *hh: "A,57,1793980812457,0.12,-0.05,9.79";
// - o arquivo é o da hora UTC do t de cada linha (no $H, o unix_ms). Quando a hora muda, para a
//   frente ou para trás, abre o arquivo dessa hora, que pode já existir, e escreve nele o
//   cabeçalho: "# V-GUARD <versão> | <colunas>". Assim, o arquivo que já existia (o ESP32
//   reiniciou) continua no fim, depois de um cabeçalho novo;
// - depois de um erro do cartão, não grava mais.
class Registro {
 public:
  Registro(Cartao &cartao, const char *versao) : cartao_(cartao), versao_(versao) {}

  // Bytes da fila: linhas inteiras terminadas em \n, que podem vir cortadas em qualquer ponto
  // entre duas chamadas.
  void receber(const char *bytes, size_t tamanho);

  // O cartão deu erro, e o registro parou de gravar.
  bool falhou() const { return falhou_; }

 private:
  void linha_completa();
  bool abrir_arquivo(uint64_t t);
  bool gravar(const char *texto, size_t tamanho);

  Cartao &cartao_;
  const char *versao_;

  // A linha que está chegando, sem o \n. Uma linha maior que o protocolo permite é descartada.
  char linha_[protocolo::tamanho_maximo + 1] = {};
  size_t tamanho_linha_ = 0;
  bool descartando_ = false;

  // O $B do boot, já sem a moldura e com o \n, guardado até a hora chegar.
  char boot_[protocolo::tamanho_maximo + 1] = {};
  size_t tamanho_boot_ = 0;

  bool tem_hora_ = false;
  bool tem_arquivo_ = false;
  // A hora do arquivo aberto, em horas desde 01/01/1970.
  uint64_t hora_ = 0;
  bool falhou_ = false;
};

}  // namespace vguard::arquivo_sd
