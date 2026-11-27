// Testes no computador dos mesmos fontes de relógio e gravação usados na ESP32.
#include "protocolo.h"
#include "arquivo_sd.h"
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace vguard;
constexpr uint64_t hora = 1767225600000ULL;

struct Arquivo { std::string nome, texto; };
class CartaoTeste : public arquivo_sd::Cartao {
 public:
  std::vector<Arquivo> arquivos;
  unsigned aberturas_autonomas = 0;
  bool erro_abrir = false, erro_escrever = false;
  bool abrir(const char *nome) override {
    if (erro_abrir) return false;
    arquivos.push_back({nome, ""});
    return true;
  }
  bool abrir_sem_hora() override {
    ++aberturas_autonomas;
    return abrir("SEM00001.csv");
  }
  bool escrever(const char *texto, size_t n) override {
    if (erro_escrever) return false;
    assert(!arquivos.empty());
    arquivos.back().texto.append(texto, n);
    return true;
  }
};

std::string linha(char tipo, uint64_t t) {
  protocolo::Linha l(tipo);
  if (tipo != 'H') l.campo_inteiro(1);
  l.campo_inteiro(t);
  if (tipo == 'A') l.campo_texto("0.00,0.00,9.81");
  assert(l.fechar());
  return {l.dados(), l.tamanho()};
}
void enviar(arquivo_sd::Registro &r, char tipo, uint64_t t) {
  const auto l = linha(tipo, t);
  // Simula a fila devolvendo até um único byte por leitura.
  for (const char c : l) r.receber(&c, 1);
}
size_t contar(const std::string &texto, const std::string &parte) {
  size_t n = 0, pos = 0;
  while ((pos = texto.find(parte, pos)) != std::string::npos) { ++n; pos += parte.size(); }
  return n;
}

void limite_120s() {
  protocolo::Relogio r;
  assert(!r.ativo() && r.t(0) == 0);
  assert(!r.ativar_autonomo(119999, 120000));
  assert(r.t(119999) == 0);
  assert(r.ativar_autonomo(120000, 120000));
  assert(r.ativo() && !r.acertado() && r.t(120010) == 120010);
  assert(!r.ativar_autonomo(120010, 120000));
}
void hora_antes_do_limite() {
  protocolo::Relogio r;
  r.acertar(hora, 119999);
  assert(!r.ativar_autonomo(120000, 120000));
  assert(r.ativo() && r.acertado() && r.t(120000) == hora + 1);
}
void hora_apos_fallback() {
  protocolo::Relogio r;
  assert(r.ativar_autonomo(120000, 120000));
  r.acertar(hora, 130000);
  assert(r.t(130050) == hora + 50);
  assert(!r.ativar_autonomo(200000, 120000));
  r.acertar(hora + 1, 200000);
  assert(r.t(200020) == hora + 21);
}
void hora_invalida_nao_ativa() {
  assert(protocolo::interpretar('H', "0").tipo == protocolo::Comando::Tipo::hora_invalida);
  protocolo::Leitor leitor;
  const std::string invalida = "$H,1767225600000*ZZ\n";
  for (const char c : invalida) assert(!leitor.receber(c));
  protocolo::Relogio r;
  assert(!r.ativar_autonomo(119999, 120000));
  assert(r.ativar_autonomo(120000, 120000));
}
void espera_nao_cria_arquivo() {
  CartaoTeste c; arquivo_sd::Registro r(c, "teste");
  enviar(r, 'B', 0); enviar(r, 'S', 0);
  assert(c.arquivos.empty() && !r.falhou());
}
void autonomo_grava_sem_data() {
  CartaoTeste c; arquivo_sd::Registro r(c, "teste");
  enviar(r, 'B', 0); enviar(r, 'S', 0);
  enviar(r, 'L', 120000); enviar(r, 'A', 120020); enviar(r, 'S', 3600001);
  assert(c.arquivos.size() == 1 && c.aberturas_autonomas == 1);
  const auto &a = c.arquivos[0];
  assert(a.nome == "SEM00001.csv");
  assert(a.texto.find("# SEM_HORA:") != std::string::npos);
  assert(contar(a.texto, "\nB,1,0\n") == 1);
  assert(a.texto.find("A,1,120020,0.00,0.00,9.81\n") != std::string::npos);
  assert(a.texto.find("S,1,3600001\n") != std::string::npos);
  assert(a.texto.find('$') == std::string::npos);
}
void transicao_preserva_dados_e_ordem() {
  CartaoTeste c; arquivo_sd::Registro r(c, "teste");
  enviar(r, 'B', 0); enviar(r, 'A', 120020);
  const std::string antes = c.arquivos[0].texto;
  enviar(r, 'H', hora); enviar(r, 'A', hora + 20);
  assert(c.arquivos.size() == 2);
  assert(c.arquivos[0].texto == antes);
  assert(c.arquivos[1].nome == "20260101-00.csv");
  assert(c.arquivos[1].texto.find("H,1767225600000\n") != std::string::npos);
  assert(c.arquivos[1].texto.find("A,1,1767225600020,") != std::string::npos);
  enviar(r, 'H', hora + 3600000);
  assert(c.arquivos.size() == 3 && c.arquivos[2].nome == "20260101-01.csv");
}
void sincronizado_mantem_fluxo_original() {
  CartaoTeste c; arquivo_sd::Registro r(c, "teste");
  enviar(r, 'B', 0); enviar(r, 'S', 0); enviar(r, 'H', hora); enviar(r, 'A', hora + 10);
  assert(c.aberturas_autonomas == 0 && c.arquivos.size() == 1);
  assert(c.arquivos[0].nome == "20260101-00.csv");
  assert(c.arquivos[0].texto.find("\nB,1,0\nH,") != std::string::npos);
}
void erros_de_abertura_e_escrita() {
  CartaoTeste c; c.erro_abrir = true;
  arquivo_sd::Registro r(c, "teste"); enviar(r, 'A', 120020);
  assert(r.falhou() && c.arquivos.empty());
  c.erro_abrir = false; enviar(r, 'H', hora);
  assert(c.arquivos.empty());
  CartaoTeste d; d.erro_escrever = true;
  arquivo_sd::Registro s(d, "teste"); enviar(s, 'A', 120020);
  assert(s.falhou());
}
void erro_ao_mudar_para_hora() {
  CartaoTeste c; arquivo_sd::Registro r(c, "teste");
  enviar(r, 'A', 120020);
  const auto antes = c.arquivos[0].texto;
  c.erro_abrir = true; enviar(r, 'H', hora);
  assert(r.falhou() && c.arquivos.size() == 1 && c.arquivos[0].texto == antes);
}
void relogio_64bits_e_reboot() {
  protocolo::Relogio r;
  constexpr uint64_t depois_49_dias = (1ULL << 32) + 500;
  assert(r.ativar_autonomo(depois_49_dias, 120000));
  assert(r.t(depois_49_dias) == depois_49_dias);
  protocolo::Relogio novo;
  assert(!novo.ativo() && novo.t(100) == 0);
}

int main() {
  limite_120s(); hora_antes_do_limite(); hora_apos_fallback(); hora_invalida_nao_ativa();
  espera_nao_cria_arquivo(); autonomo_grava_sem_data(); transicao_preserva_dados_e_ordem();
  sincronizado_mantem_fluxo_original(); erros_de_abertura_e_escrita();
  erro_ao_mudar_para_hora(); relogio_64bits_e_reboot();
  std::puts("OK: 11 cenarios de fallback, SD e sincronizacao tardia.");
}
