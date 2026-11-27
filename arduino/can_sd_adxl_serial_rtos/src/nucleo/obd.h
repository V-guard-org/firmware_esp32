// OBD-II pelo CAN (proposta §4.3): os quadros do serviço 01, a descoberta dos PIDs suportados e
// a leitura dos 8 PIDs de interesse em rodízio. Só memória de tamanho fixo.
//
// A máquina não chama o hardware. A parte do ESP32 avisa o que aconteceu (chegou um quadro,
// passaram 10 ms) e a máquina responde pela Saida: mudar o modo do MCP2515, mandar um pedido,
// emitir um $O ou um $P. Assim ela se testa no notebook, com quadros e horários inventados.
//
// As funções puras vêm do obd_core.h do Nicolas (ramo leitura-e-teste-gps/lte-arduino-mega do
// firmware_telemetria, PR #39), com os nomes em português. A máquina segue a do voyage_painel.ino
// (repositório OBD-II), validada no Voyage 2015.
#pragma once

#include <cstddef>
#include <cstdint>

#include "protocolo.h"

namespace vguard::obd {

// IDs do Voyage 2015 (proposta §4.3): o primeiro mapa vai para todos, o resto para a central, e
// ela responde em 0x7E8.
constexpr uint16_t id_todos = 0x7DF;
constexpr uint16_t id_central = 0x7E0;
constexpr uint16_t id_resposta = 0x7E8;

// Tempo limite de cada pedido (proposta §4.3).
constexpr uint32_t tempo_limite_ms = 1200;
// A descoberta só começa com tráfego no CAN nos últimos 2 s.
constexpr uint32_t trafego_recente_ms = 2000;
// Regras de falha do voyage_painel.ino (proposta §4.3): um PID que falha 3 vezes fica suspenso
// até a próxima descoberta; 3 pedidos seguidos sem resposta fazem parar; depois de parar, uma
// nova descoberta a cada 15 s, contados da parada.
constexpr uint8_t falhas_para_suspender = 3;
constexpr uint8_t falhas_seguidas_para_parar = 3;
constexpr uint32_t intervalo_tentativas_ms = 15000;
// O "aguarde" (03 7F 01 78): a central pede mais tempo. Não conta como falha.
constexpr uint8_t codigo_aguarde = 0x78;
// O CAN é lido a cada 10 ms. Um atraso menor que isso é só o da leitura, e a agenda dos pedidos
// segue; um atraso maior é da central, e a agenda recomeça do envio.
constexpr uint32_t folga_da_agenda_ms = 10;

// --- Copiadas do obd_core.h, com os nomes em português ---

// obdStandardFilter: máscara ou filtro de ID de 11 bits no formato do mcp_can 1.5.1 (SID nos
// bits 26 a 16; os 16 de baixo filtram os primeiros bytes de dados).
constexpr uint32_t filtro_padrao(uint16_t id) { return uint32_t(id & 0x7FF) << 16; }

// obdTimedOut: o limite passou desde o envio, também quando o contador de ms dá a volta. Um
// instante anterior ao envio não expira.
constexpr bool tempo_esgotado(uint32_t agora, uint32_t enviado, uint32_t limite) {
  return int32_t(agora - enviado) >= int32_t(limite);
}

// obdPositive: resposta positiva do serviço 01 ao PID, num quadro único do ISO-TP. O id vem como
// o mcp_can entrega, com as marcas de quadro estendido e de RTR, que fazem recusar. Em `bytes`,
// quantos bytes de dados o PID trouxe.
constexpr bool resposta_positiva(uint32_t id, uint8_t dlc, const uint8_t *d, uint32_t central,
                                 uint8_t pid, uint8_t &bytes) {
  if (id != central || dlc < 4 || dlc > 8 || (d[0] & 0xF0) || d[0] < 3 || d[0] > 7 ||
      d[0] + 1 > dlc || d[1] != 0x41 || d[2] != pid) {
    return false;
  }
  bytes = d[0] - 2;
  return true;
}

// Resposta negativa do serviço 01 (03 7F 01 <código>), como o tratarResposta() do
// voyage_painel.ino a reconhece. Ela não traz o PID: vale para o pedido em andamento. O código
// 0x78 é o "aguarde"; os outros são recusa.
constexpr bool resposta_negativa(uint32_t id, uint8_t dlc, const uint8_t *d, uint32_t central,
                                 uint8_t &codigo) {
  if (id != central || dlc < 4 || dlc > 8 || d[0] != 3 || d[1] != 0x7F || d[2] != 1) {
    return false;
  }
  codigo = d[3];
  return true;
}

// obdSupported: o mapa anuncia o PID na posição `deslocamento` (1 a 32) a partir do PID do mapa.
constexpr bool suportado(uint32_t mapa, uint8_t deslocamento) {
  return deslocamento >= 1 && deslocamento <= 32 &&
         (mapa & (uint32_t(1) << (32 - deslocamento))) != 0;
}

// obdMap: os 4 bytes do mapa, do mais significativo para o menos.
constexpr uint32_t mapa(const uint8_t *d) {
  return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) |
         uint32_t(d[3]);
}

// mapPid: o PID é um dos mapas (00, 20, 40...).
constexpr bool pid_de_mapa(uint8_t pid) { return pid == 0 || (pid % 32) == 0; }

// expectedBytes: bytes de dados da resposta do PID; 0 se ele não é conhecido.
constexpr uint8_t bytes_esperados(uint8_t pid) {
  switch (pid) {
    case 0x0C: case 0x10: case 0x1F: case 0x21:
    case 0x31: case 0x42: case 0x5E: return 2;
    case 0x04: case 0x05: case 0x06: case 0x07: case 0x0B:
    case 0x0D: case 0x0E: case 0x0F: case 0x11: case 0x2F:
    case 0x33: case 0x46: case 0x52: case 0x5C: return 1;
    default: return 0;
  }
}

// obdDecode: o valor do PID, a partir dos bytes de dados da resposta.
constexpr bool decodificar(uint8_t pid, const uint8_t *d, uint8_t bytes, float &v) {
  const uint8_t esperados = bytes_esperados(pid);
  if (!esperados || bytes != esperados) {
    return false;
  }
  const uint16_t ab = esperados == 2 ? uint16_t((uint16_t(d[0]) << 8) | d[1]) : d[0];
  switch (pid) {
    case 0x0C: v = ab / 4.0f; break;
    case 0x0D: case 0x0B: case 0x33: v = d[0]; break;
    case 0x05: case 0x0F: case 0x46: case 0x5C: v = int(d[0]) - 40; break;
    case 0x06: case 0x07: v = d[0] / 1.28f - 100; break;
    case 0x0E: v = d[0] / 2.0f - 64; break;
    case 0x10: v = ab / 100.0f; break;
    case 0x1F: case 0x21: case 0x31: v = ab; break;
    case 0x42: v = ab / 1000.0f; break;
    case 0x5E: v = ab / 20.0f; break;
    default: v = d[0] * 100.0f / 255; break;
  }
  return true;
}

// --- Os 8 PIDs de interesse (proposta §4.3 e §5.3) ---

// O campo do $O e as casas decimais do valor.
struct Campo {
  uint8_t pid;
  const char *nome;
  uint8_t casas;
};

constexpr Campo campos[] = {
    {0x04, "load", 1},  {0x05, "coolant", 0}, {0x0B, "map", 0},      {0x0C, "rpm", 2},
    {0x0D, "speed", 0}, {0x0F, "iat", 0},     {0x11, "throttle", 1}, {0x42, "voltage", 3},
};
constexpr size_t quantidade_campos = sizeof(campos) / sizeof(campos[0]);

// O valor decodificado, multiplicado por 10^casas e arredondado: o que o $O mostra. rpm 876,0
// dá 87600 (876.00); load 52,16 dá 522 (52.2).
int64_t valor_escalado(float valor, uint8_t casas);

// Um quadro do CAN, como o mcp_can entrega: o id com as marcas de estendido e de RTR.
struct Quadro {
  uint32_t id;
  uint8_t dlc;
  uint8_t dados[8];
};

// O que a máquina pede à parte do ESP32.
class Saida {
 public:
  // Antes da descoberta: o MCP2515 sai do só escuta para o modo normal, aceitando só 0x7E8.
  // Devolve false se ele não respondeu.
  virtual bool configurar_leitura() = 0;
  // Ao parar: o MCP2515 é iniciado de novo em só escuta, sem filtros, o que zera os erros do CAN.
  // Devolve false se ele não respondeu.
  virtual bool voltar_a_escutar() = 0;
  // Manda um pedido, com envio único e DLC 8.
  virtual void enviar(uint16_t id, const uint8_t (&dados)[8]) = 0;
  // $O,<seq>,<t>,<campo>,<valor>, com o valor já escalado pelas casas.
  virtual void emitir_obd(const char *campo, int64_t valor_escalado, uint8_t casas) = 0;
  // $P,<seq>,<t>,<mapa00>,<mapa20>,<mapa40>, depois de cada descoberta.
  virtual void emitir_pids(uint32_t mapa00, uint32_t mapa20, uint32_t mapa40) = 0;
  // $L,<seq>,<t>,AVISO,<texto>: um PID suspenso ou a leitura que parou.
  virtual void emitir_aviso(const char *texto) = 0;

 protected:
  ~Saida() = default;
};

// A máquina do OBD. Os horários são os ms desde o boot, que podem dar a volta.
//
// - ESCUTANDO: o MCP2515 só escuta. Com tráfego nos últimos 2 s, começa a descoberta;
// - DESCOBRINDO: pede os mapas 00 (a 0x7DF), 20 e 40 (a 0x7E0), enquanto cada um anuncia o
//   seguinte. No fim, emite o $P;
// - LENDO: pede os PIDs de interesse que o carro suporta, em rodízio, e emite um $O a cada
//   resposta;
// - SEM_RESPOSTA: parou, e há tráfego no CAN nos últimos 2 s. Depois de parar, o obd é
//   SEM_RESPOSTA ou ESCUTANDO conforme o tráfego, e a nova descoberta sai 15 s depois da parada,
//   com tráfego visto desde ela;
// - FALHA: o MCP2515 não voltou a só escuta. Fica assim até o próximo boot.
// Um pedido por vez, no ritmo da taxa do $C, por uma agenda fixa: o próximo é marcado a partir do
// horário planejado do anterior. Se a resposta atrasa, o próximo espera por ela, e a agenda
// recomeça do envio.
//
// Falhas, como no voyage_painel.ino (proposta §4.3): o tempo limite (1200 ms) esgotado e a
// recusa contam; o "aguarde" não, e o pedido segue esperando até o tempo limite.
// - Na leitura, cada falha conta para o PID, e uma resposta positiva dele zera a conta. Com 3, ele
//   fica suspenso até a próxima descoberta. Sem nenhum PID ativo, para.
// - As falhas seguidas, de qualquer PID, também contam, e qualquer resposta positiva zera a conta.
//   Com 3, para, mas só quando a 3ª é um pedido sem resposta: a recusa entra na conta e não para.
// - Na descoberta, a falha do mapa 00 faz parar; a do 20 ou do 40 encerra a descoberta com o que
//   já se sabe.
// - Para também com erro no CAN (erro passivo ou bus-off) e quando o MCP2515 não aceita os filtros.
// Os avisos: "PID 0F sem resposta" ou "PID 0F recusado" ao suspender, e "OBD sem resposta", "OBD
// erro no CAN" ou "OBD sem PID ativo" ao parar. O aviso de parada sai um por episódio: as novas
// tentativas que falham não o repetem, e a próxima descoberta que dá certo (o $P) encerra o
// episódio.
class Maquina {
 public:
  explicit Maquina(Saida &saida) : saida_(saida) {}

  // Entrou em ATIVO, com o MCP2515 iniciado em só escuta.
  void comecar(uint32_t agora);
  // Chegou um quadro do CAN.
  void receber(const Quadro &quadro, uint32_t agora);
  // O MCP2515 acusou erro passivo ou bus-off. Só vale enquanto pede (pedindo()).
  void erro_no_can(uint32_t agora);
  // A cada 10 ms. obd_hz_decimos: a taxa do $C, em décimos de pedido por segundo.
  void tique(uint32_t agora, uint16_t obd_hz_decimos);

  // O obd do $S: PARADO antes do comecar(), depois ESCUTANDO, DESCOBRINDO, LENDO, SEM_RESPOSTA
  // ou FALHA.
  protocolo::EstadoObd estado() const { return estado_; }
  // Descobrindo ou lendo: o MCP2515 está no modo normal, e o erro no CAN conta.
  bool pedindo() const;

 private:
  enum class Falha : uint8_t { sem_resposta, recusa };
  enum class Motivo : uint8_t { sem_resposta, erro_no_can, sem_pid_ativo };

  void comecar_descoberta(uint32_t agora);
  void pedir(uint8_t pid, uint32_t agora, uint16_t obd_hz_decimos);
  bool pedir_proximo_pid(uint32_t agora, uint16_t obd_hz_decimos);
  void responder_mapa(const uint8_t *dados_do_mapa);
  void terminar_descoberta();
  void marcar_proximo(uint32_t agora, uint16_t obd_hz_decimos);
  void falhar(Falha falha, uint32_t agora);
  void parar(Motivo motivo, uint32_t agora);
  void avisar_pid_suspenso(uint8_t pid, Falha falha);
  bool trafego_recente(uint32_t agora) const;

  Saida &saida_;
  protocolo::EstadoObd estado_ = protocolo::EstadoObd::parado;

  bool viu_trafego_ = false;
  uint32_t ultimo_trafego_ = 0;

  // A última parada: as novas descobertas saem a cada 15 s contados dela.
  bool parou_ = false;
  uint32_t parou_em_ = 0;
  // O aviso de parada deste episódio já saiu.
  bool avisou_parada_ = false;
  // Falhas seguidas, de qualquer PID.
  uint8_t falhas_seguidas_ = 0;

  // O pedido em andamento, e na leitura o índice dele em `campos`.
  bool aguardando_ = false;
  uint8_t pendente_ = 0;
  uint8_t campo_pendente_ = 0;
  uint32_t enviado_em_ = 0;

  // A agenda: o próximo pedido sai em proximo_ms_ e mais fracao_us_.
  uint32_t proximo_ms_ = 0;
  uint32_t fracao_us_ = 0;

  // Descoberta: o mapa da vez (0x00, 0x20 ou 0x40) e os três mapas, para o $P.
  uint8_t mapa_atual_ = 0;
  uint32_t mapas_[3] = {};

  // Leitura: os índices em `campos` dos PIDs suportados, e o próximo do rodízio.
  uint8_t ativos_[quantidade_campos] = {};
  uint8_t quantidade_ativos_ = 0;
  uint8_t proximo_ativo_ = 0;
  // Por índice em `campos`: as falhas do PID desde a última resposta positiva dele, e se ele
  // está suspenso até a próxima descoberta.
  uint8_t falhas_[quantidade_campos] = {};
  bool suspenso_[quantidade_campos] = {};
};

}  // namespace vguard::obd
