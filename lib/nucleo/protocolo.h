// Protocolo da serial ESP32 ↔ Pi (proposta §5): a moldura $<tipo>,<campos...>*<hh>, o
// checksum e as linhas que o ESP32 manda. Só memória de tamanho fixo.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vguard::protocolo {

// Maior linha aceita, do $ ao último dígito do checksum, sem o \r\n (proposta §5.1).
constexpr size_t tamanho_maximo = 120;

// O XOR de todos os caracteres do texto: o checksum do NMEA. Na moldura, vai do caractere
// depois do $ até o anterior ao *.
uint8_t checksum(const char *texto, size_t tamanho);

// Monta uma linha num buffer fixo. Cada campo entra depois de uma vírgula:
//
//     Linha linha('S');
//     linha.campo_inteiro(seq).campo_texto("ESPERA");
//     linha.fechar();  // "$S,<seq>,ESPERA*hh\n"
//
// A linha fica inválida se passar do tamanho máximo ou se um texto tiver $, * ou quebra de
// linha. Aí o fechar() devolve false, e ela não deve ser enviada.
class Linha {
 public:
  explicit Linha(char tipo);

  Linha &campo_texto(const char *texto);
  Linha &campo_inteiro(uint64_t valor);
  // Número com casas decimais fixas, a partir do valor multiplicado por 10^casas:
  // (87625, 2) dá 876.25, e (-5, 2) dá -0.05.
  Linha &campo_decimal(int64_t valor_escalado, uint8_t casas);

  // Fecha a moldura com *hh e \n. Devolve false se a linha é inválida.
  bool fechar();

  // A linha, terminada em \0. Depois do fechar(), inclui o *hh\n.
  const char *dados() const { return buffer_; }
  // Em caracteres, sem o \0. Depois do fechar(), inclui o *hh\n.
  size_t tamanho() const { return tamanho_; }

 private:
  void virgula();
  void acrescentar(char caractere);
  void acrescentar_digitos(uint64_t valor, uint8_t minimo);

  // A linha mais o \n e o \0.
  char buffer_[tamanho_maximo + 2] = {};
  size_t tamanho_ = 0;
  bool valida_ = true;
  bool fechada_ = false;
};

// Motivo do último reinício, no $B (proposta §5.3). QUEDA_TENSAO mostra o reinício na
// partida do motor.
enum class MotivoReset : uint8_t { ligou, software, pane, watchdog, queda_tensao, outro };

// Campos do $S (proposta §5.3).
enum class Estado : uint8_t { espera, ativo };
enum class EstadoObd : uint8_t { parado, escutando, descobrindo, lendo, sem_resposta, falha };
enum class EstadoImu : uint8_t { ok, falha };
enum class EstadoSd : uint8_t { ok, ausente, cheio, falha };

// Os nomes que vão na linha: LIGOU, ESPERA, SEM_RESPOSTA etc.
const char *nome(MotivoReset motivo);
const char *nome(Estado estado);
const char *nome(EstadoObd obd);
const char *nome(EstadoImu imu);
const char *nome(EstadoSd sd);

// O que o $S informa, menos a seq, o t e as descartadas, que vêm do Emissor.
struct Status {
  uint32_t uptime_s;
  Estado estado;
  EstadoObd obd;
  EstadoImu imu;
  EstadoSd sd;
  // Espaço livre no cartão, em %.
  uint8_t sd_livre;
  // Taxa do OBD em uso, em décimos de pedido por segundo: 125 é 12.5.
  uint16_t obd_hz_decimos;
};

// Para onde vão as linhas emitidas. A parte do ESP32 implementa sobre a saída da serial.
class Destino {
 public:
  // Põe a linha inteira na saída, sem esperar. Devolve false se ela não coube.
  virtual bool escrever(const char *linha, size_t tamanho) = 0;

 protected:
  ~Destino() = default;
};

// Monta as linhas do ESP32, com a seq, e as põe no destino (proposta §5.2 e §5.5).
//
// Toda linha gasta uma seq, mesmo a que não coube no destino. Assim a lacuna que a Pi vê é a
// linha que não chegou, e as linhas do SD, que guarda todas, não repetem seq.
//
// Não é seguro entre tarefas: quem usa de mais de uma tarefa protege com uma trava.
class Emissor {
 public:
  explicit Emissor(Destino &destino) : destino_(destino) {}

  // $B,<seq>,<t>,<firmware>,<motivo_reset>
  void emitir_boot(uint64_t t, const char *firmware, MotivoReset motivo);
  // $S,<seq>,<t>,<uptime_s>,<estado>,<obd>,<imu>,<sd>,<sd_livre>,<obd_hz>,<descartadas>
  void emitir_status(uint64_t t, const Status &status);

  // Linhas que não couberam no destino desde o boot.
  uint32_t descartadas() const { return descartadas_; }

 private:
  // Começa a linha com o tipo, a seq e o t.
  Linha comecar(char tipo, uint64_t t);
  void emitir(Linha &linha);

  Destino &destino_;
  uint32_t proxima_seq_ = 0;
  uint32_t descartadas_ = 0;
};

}  // namespace vguard::protocolo
