// Testes da máquina do OBD (lib/nucleo/obd), com quadros e horários inventados: a descoberta, o
// rodízio dos 8 PIDs, a decodificação, o ritmo da agenda e o tempo limite (proposta §4.3 e §4.7).
#include <unity.h>

#include <cstring>

#include "obd.h"

using namespace vguard::obd;
using vguard::protocolo::EstadoObd;

namespace {

constexpr size_t maximo = 512;

struct Pedido {
  uint16_t id;
  uint8_t dados[8];
  uint32_t em;
};

struct LinhaObd {
  char campo[16];
  int64_t valor;
  uint8_t casas;
};

// Guarda o que a máquina pediu.
class SaidaDeTeste final : public Saida {
 public:
  void configurar_leitura() override { ++configuracoes; }
  void enviar(uint16_t id, const uint8_t (&dados)[8]) override {
    if (pedidos_feitos < maximo) {
      Pedido &pedido = pedidos[pedidos_feitos];
      pedido.id = id;
      std::memcpy(pedido.dados, dados, sizeof(dados));
      pedido.em = agora;
    }
    ++pedidos_feitos;
  }
  void emitir_obd(const char *campo, int64_t valor, uint8_t casas) override {
    if (leituras_feitas < maximo) {
      LinhaObd &linha = leituras[leituras_feitas];
      std::strncpy(linha.campo, campo, sizeof(linha.campo) - 1);
      linha.valor = valor;
      linha.casas = casas;
    }
    ++leituras_feitas;
  }
  void emitir_pids(uint32_t mapa00, uint32_t mapa20, uint32_t mapa40) override {
    pids[0] = mapa00;
    pids[1] = mapa20;
    pids[2] = mapa40;
    ++pids_emitidos;
  }

  uint32_t agora = 0;
  int configuracoes = 0;
  Pedido pedidos[maximo] = {};
  size_t pedidos_feitos = 0;
  LinhaObd leituras[maximo] = {};
  size_t leituras_feitas = 0;
  uint32_t pids[3] = {};
  int pids_emitidos = 0;
};

Quadro quadro(uint32_t id, const uint8_t (&dados)[8]) {
  Quadro q{id, 8, {}};
  std::memcpy(q.dados, dados, sizeof(dados));
  return q;
}

// Um quadro de fundo, com outro ID: o tráfego que faz a descoberta começar.
const Quadro fundo = quadro(0x123, {1, 2, 3, 4, 5, 6, 7, 8});

// Os valores do simulador (proposta §4.7).
Quadro valor_do_simulador(uint8_t pid) {
  switch (pid) {
    case 0x04: return quadro(id_resposta, {0x03, 0x41, 0x04, 0x85, 0x55, 0x55, 0x55, 0x55});
    case 0x05: return quadro(id_resposta, {0x03, 0x41, 0x05, 0x57, 0x55, 0x55, 0x55, 0x55});
    case 0x0B: return quadro(id_resposta, {0x03, 0x41, 0x0B, 0x21, 0x55, 0x55, 0x55, 0x55});
    case 0x0C: return quadro(id_resposta, {0x04, 0x41, 0x0C, 0x0D, 0xB0, 0x55, 0x55, 0x55});
    case 0x0D: return quadro(id_resposta, {0x03, 0x41, 0x0D, 0x3C, 0x55, 0x55, 0x55, 0x55});
    case 0x0F: return quadro(id_resposta, {0x03, 0x41, 0x0F, 0x46, 0x55, 0x55, 0x55, 0x55});
    case 0x11: return quadro(id_resposta, {0x03, 0x41, 0x11, 0x26, 0x55, 0x55, 0x55, 0x55});
    case 0x42: return quadro(id_resposta, {0x04, 0x41, 0x42, 0x35, 0xE8, 0x55, 0x55, 0x55});
    default: return quadro(0x7FF, {});
  }
}

Quadro resposta_de_mapa(uint8_t pid, uint32_t valor) {
  return quadro(id_resposta, {0x06, 0x41, pid, static_cast<uint8_t>(valor >> 24),
                              static_cast<uint8_t>(valor >> 16), static_cast<uint8_t>(valor >> 8),
                              static_cast<uint8_t>(valor), 0x55});
}

// O firmware e uma central simulada: a cada 10 ms, entrega a resposta que chegou e chama o
// tique, como a tarefa de aquisição.
class Bancada {
 public:
  explicit Bancada(uint32_t inicio = 0) : agora(inicio) { saida.agora = inicio; }

  // Entra em ATIVO e vê um quadro de fundo.
  void ligar() {
    maquina.comecar(agora);
    maquina.receber(fundo, agora);
  }

  void passar(uint32_t ms) {
    const uint32_t fim = agora + ms;
    while (int32_t(fim - agora) > 0) {
      if (tem_resposta && int32_t(agora - resposta_em) >= 0) {
        tem_resposta = false;
        maquina.receber(resposta, agora);
      }
      saida.agora = agora;
      const size_t antes = saida.pedidos_feitos;
      maquina.tique(agora, obd_hz_decimos);
      if (saida.pedidos_feitos > antes && responde) {
        responder(saida.pedidos[(saida.pedidos_feitos - 1) % maximo]);
      }
      agora += 10;
    }
  }

  // Até o fim da descoberta (o $P).
  void descobrir() {
    for (int i = 0; i < 100 && saida.pids_emitidos == 0; ++i) {
      passar(10);
    }
  }

  // Os pedidos feitos depois do pedido `primeiro`.
  size_t pedidos_desde(size_t primeiro) const { return saida.pedidos_feitos - primeiro; }

  uint32_t agora;
  SaidaDeTeste saida;
  Maquina maquina{saida};
  uint16_t obd_hz_decimos = 125;
  uint32_t atraso_ms = 10;
  bool responde = true;
  uint32_t mapas[3] = {0x183A8001UL, 0x00000001UL, 0x40000000UL};

 private:
  void responder(const Pedido &pedido) {
    const uint8_t pid = pedido.dados[2];
    resposta =
        pid_de_mapa(pid) ? resposta_de_mapa(pid, mapas[pid / 0x20]) : valor_do_simulador(pid);
    resposta_em = agora + atraso_ms;
    tem_resposta = true;
  }

  bool tem_resposta = false;
  Quadro resposta{};
  uint32_t resposta_em = 0;
};

void conferir_pedido(const Pedido &pedido, uint16_t id, uint8_t pid) {
  const uint8_t esperado[8] = {0x02, 0x01, pid, 0, 0, 0, 0, 0};
  TEST_ASSERT_EQUAL_HEX16(id, pedido.id);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(esperado, pedido.dados, 8);
}

}  // namespace

void setUp() {}

void tearDown() {}

static void teste_parado_antes_de_comecar() {
  Bancada bancada;
  bancada.maquina.receber(fundo, 0);
  bancada.passar(1000);
  TEST_ASSERT_EQUAL(EstadoObd::parado, bancada.maquina.estado());
  TEST_ASSERT_EQUAL_UINT(0, bancada.saida.pedidos_feitos);
}

static void teste_escuta_sem_trafego() {
  Bancada bancada;
  bancada.maquina.comecar(0);
  bancada.passar(5000);
  TEST_ASSERT_EQUAL(EstadoObd::escutando, bancada.maquina.estado());
  TEST_ASSERT_EQUAL_UINT(0, bancada.saida.pedidos_feitos);
  TEST_ASSERT_EQUAL_INT(0, bancada.saida.configuracoes);
}

static void teste_trafego_de_mais_de_2_s_nao_comeca() {
  Bancada bancada;
  bancada.maquina.comecar(0);
  bancada.maquina.receber(fundo, 0);
  bancada.maquina.tique(2001, 125);
  TEST_ASSERT_EQUAL(EstadoObd::escutando, bancada.maquina.estado());
  TEST_ASSERT_EQUAL_UINT(0, bancada.saida.pedidos_feitos);
}

static void teste_trafego_comeca_a_descoberta_pelo_mapa_00() {
  Bancada bancada;
  bancada.maquina.comecar(0);
  bancada.maquina.receber(fundo, 5);
  bancada.maquina.tique(2005, 125);
  TEST_ASSERT_EQUAL(EstadoObd::descobrindo, bancada.maquina.estado());
  TEST_ASSERT_EQUAL_INT(1, bancada.saida.configuracoes);
  TEST_ASSERT_EQUAL_UINT(1, bancada.saida.pedidos_feitos);
  conferir_pedido(bancada.saida.pedidos[0], 0x7DF, 0x00);
}

static void teste_descoberta_com_os_mapas_do_simulador() {
  Bancada bancada;
  bancada.ligar();
  bancada.descobrir();
  TEST_ASSERT_EQUAL_INT(1, bancada.saida.pids_emitidos);
  TEST_ASSERT_EQUAL_HEX32(0x183A8001UL, bancada.saida.pids[0]);
  TEST_ASSERT_EQUAL_HEX32(0x00000001UL, bancada.saida.pids[1]);
  TEST_ASSERT_EQUAL_HEX32(0x40000000UL, bancada.saida.pids[2]);
  TEST_ASSERT_EQUAL(EstadoObd::lendo, bancada.maquina.estado());
  TEST_ASSERT_EQUAL_UINT(3, bancada.saida.pedidos_feitos);
  conferir_pedido(bancada.saida.pedidos[0], 0x7DF, 0x00);
  conferir_pedido(bancada.saida.pedidos[1], 0x7E0, 0x20);
  conferir_pedido(bancada.saida.pedidos[2], 0x7E0, 0x40);
  // Os mapas também seguem o ritmo: um a cada 80 ms.
  TEST_ASSERT_EQUAL_UINT32(80, bancada.saida.pedidos[1].em - bancada.saida.pedidos[0].em);
  TEST_ASSERT_EQUAL_UINT32(80, bancada.saida.pedidos[2].em - bancada.saida.pedidos[1].em);
}

static void teste_mapa_sem_o_seguinte_termina_a_descoberta() {
  Bancada bancada;
  bancada.mapas[0] = 0x183A8000UL;  // sem o bit 32: não há mapa 20
  bancada.ligar();
  bancada.descobrir();
  TEST_ASSERT_EQUAL_UINT(1, bancada.saida.pedidos_feitos);
  TEST_ASSERT_EQUAL_HEX32(0x183A8000UL, bancada.saida.pids[0]);
  TEST_ASSERT_EQUAL_HEX32(0, bancada.saida.pids[1]);
  TEST_ASSERT_EQUAL_HEX32(0, bancada.saida.pids[2]);
  // Sem o mapa 40, o 42 (voltage) fica de fora: 7 PIDs no rodízio.
  const size_t primeiro = bancada.saida.pedidos_feitos;
  bancada.passar(80 * 14);
  for (size_t i = primeiro; i < bancada.saida.pedidos_feitos; ++i) {
    TEST_ASSERT_NOT_EQUAL(0x42, bancada.saida.pedidos[i].dados[2]);
  }
}

static void teste_le_os_8_em_rodizio_com_os_valores_da_tabela() {
  Bancada bancada;
  bancada.ligar();
  bancada.descobrir();
  bancada.passar(80 * 9 + 20);
  // A tabela do ticket: o campo, o valor escalado e as casas.
  const LinhaObd esperadas[] = {
      {"load", 522, 1},   {"coolant", 47, 0}, {"map", 33, 0},       {"rpm", 87600, 2},
      {"speed", 60, 0},   {"iat", 30, 0},     {"throttle", 149, 1}, {"voltage", 13800, 3},
      {"load", 522, 1},
  };
  TEST_ASSERT_EQUAL_UINT(9, bancada.saida.leituras_feitas);
  for (size_t i = 0; i < 9; ++i) {
    TEST_ASSERT_EQUAL_STRING(esperadas[i].campo, bancada.saida.leituras[i].campo);
    TEST_ASSERT_EQUAL_INT64(esperadas[i].valor, bancada.saida.leituras[i].valor);
    TEST_ASSERT_EQUAL_UINT8(esperadas[i].casas, bancada.saida.leituras[i].casas);
  }
  // Depois dos 3 mapas, os pedidos vão a 0x7E0.
  conferir_pedido(bancada.saida.pedidos[3], 0x7E0, 0x04);
  conferir_pedido(bancada.saida.pedidos[10], 0x7E0, 0x42);
}

static void teste_pid_nao_suportado_fica_de_fora() {
  Bancada bancada;
  bancada.mapas[0] = 0x183A0001UL;  // sem o 11 (throttle)
  bancada.ligar();
  bancada.descobrir();
  const size_t primeiro = bancada.saida.pedidos_feitos;
  bancada.passar(80 * 14);
  TEST_ASSERT_EQUAL_UINT(14, bancada.pedidos_desde(primeiro));
  for (size_t i = primeiro; i < bancada.saida.pedidos_feitos; ++i) {
    TEST_ASSERT_NOT_EQUAL(0x11, bancada.saida.pedidos[i].dados[2]);
  }
}

static void conferir_taxa(uint16_t decimos, uint32_t segundos, size_t esperados,
                          uint32_t menor_intervalo, uint32_t maior_intervalo) {
  Bancada bancada;
  bancada.obd_hz_decimos = decimos;
  bancada.ligar();
  bancada.descobrir();
  const size_t primeiro = bancada.saida.pedidos_feitos;
  bancada.passar(segundos * 1000);
  TEST_ASSERT_UINT_WITHIN(1, esperados, bancada.pedidos_desde(primeiro));
  for (size_t i = primeiro + 1; i < bancada.saida.pedidos_feitos; ++i) {
    const uint32_t intervalo = bancada.saida.pedidos[i].em - bancada.saida.pedidos[i - 1].em;
    TEST_ASSERT_TRUE(intervalo >= menor_intervalo && intervalo <= maior_intervalo);
  }
}

static void teste_taxa_padrao_um_a_cada_80_ms() { conferir_taxa(125, 4, 50, 80, 80); }

static void teste_taxa_de_15_pela_agenda_sai_exata() {
  // 66,7 ms não é múltiplo de 10: os intervalos alternam 60 e 70 ms, e a média dá 15.
  conferir_taxa(150, 6, 90, 60, 70);
}

static void teste_taxa_de_5() { conferir_taxa(50, 6, 30, 200, 200); }

static void teste_taxa_de_20() { conferir_taxa(200, 3, 60, 50, 50); }

static void teste_central_lenta_sem_rajada() {
  // A central leva 100 ms, mais que os 80 ms: cada pedido espera a resposta, e a agenda
  // recomeça dela, sem mandar os atrasados juntos.
  Bancada bancada;
  bancada.atraso_ms = 100;
  bancada.ligar();
  bancada.descobrir();
  const size_t primeiro = bancada.saida.pedidos_feitos;
  bancada.passar(3000);
  TEST_ASSERT_UINT_WITHIN(1, 30, bancada.pedidos_desde(primeiro));
  for (size_t i = primeiro + 1; i < bancada.saida.pedidos_feitos; ++i) {
    TEST_ASSERT_EQUAL_UINT32(100, bancada.saida.pedidos[i].em - bancada.saida.pedidos[i - 1].em);
  }
}

static void teste_nova_taxa_vale_no_proximo_pedido() {
  Bancada bancada;
  bancada.ligar();
  bancada.descobrir();
  bancada.passar(1000);
  bancada.obd_hz_decimos = 50;
  bancada.passar(200);  // o pedido marcado na taxa antiga
  const size_t primeiro = bancada.saida.pedidos_feitos;
  bancada.passar(2000);
  TEST_ASSERT_UINT_WITHIN(1, 10, bancada.pedidos_desde(primeiro));
}

static void teste_quadro_que_nao_e_a_resposta_e_ignorado() {
  Bancada bancada;
  bancada.ligar();
  bancada.descobrir();
  bancada.responde = false;
  bancada.passar(80);  // pede o 04 (load)
  const size_t pedidos = bancada.saida.pedidos_feitos;
  const Quadro errados[] = {
      quadro(0x7E9, {0x03, 0x41, 0x04, 0x85, 0x55, 0x55, 0x55, 0x55}),  // outra central
      quadro(0x7E8, {0x03, 0x41, 0x05, 0x57, 0x55, 0x55, 0x55, 0x55}),  // outro PID
      quadro(0x7E8, {0x03, 0x7F, 0x01, 0x78, 0x55, 0x55, 0x55, 0x55}),  // "aguarde"
      quadro(0x7E8, {0x03, 0x7F, 0x01, 0x12, 0x55, 0x55, 0x55, 0x55}),  // recusa (#7)
      quadro(0x7E8, {0x04, 0x41, 0x04, 0x85, 0x00, 0x55, 0x55, 0x55}),  // tamanho errado
      quadro(0x800007E8UL, {0x03, 0x41, 0x04, 0x85, 0x55, 0x55, 0x55, 0x55}),  // estendido
  };
  for (const Quadro &errado : errados) {
    bancada.maquina.receber(errado, bancada.agora);
  }
  bancada.passar(500);
  TEST_ASSERT_EQUAL_UINT(0, bancada.saida.leituras_feitas);
  TEST_ASSERT_EQUAL_UINT(pedidos, bancada.saida.pedidos_feitos);  // ainda esperando
  bancada.maquina.receber(valor_do_simulador(0x04), bancada.agora);
  TEST_ASSERT_EQUAL_UINT(1, bancada.saida.leituras_feitas);
  TEST_ASSERT_EQUAL_STRING("load", bancada.saida.leituras[0].campo);
}

static void teste_resposta_sem_pedido_e_ignorada() {
  Bancada bancada;
  bancada.ligar();
  bancada.descobrir();
  bancada.passar(30);  // a resposta do mapa 40 já chegou, e o próximo pedido ainda não saiu
  bancada.maquina.receber(valor_do_simulador(0x04), bancada.agora);
  TEST_ASSERT_EQUAL_UINT(0, bancada.saida.leituras_feitas);
}

static void teste_tempo_limite_passa_ao_proximo_pid() {
  Bancada bancada;
  bancada.ligar();
  bancada.descobrir();
  bancada.responde = false;
  bancada.passar(80);
  const Pedido load = bancada.saida.pedidos[bancada.saida.pedidos_feitos - 1];
  TEST_ASSERT_EQUAL_HEX8(0x04, load.dados[2]);
  // Até 1199 ms, espera; nos 1200 ms, pede o próximo (05, coolant).
  bancada.passar(load.em + 1199 - bancada.agora);
  TEST_ASSERT_EQUAL_UINT32(load.em, bancada.saida.pedidos[bancada.saida.pedidos_feitos - 1].em);
  bancada.passar(10);
  const Pedido coolant = bancada.saida.pedidos[bancada.saida.pedidos_feitos - 1];
  conferir_pedido(coolant, 0x7E0, 0x05);
  TEST_ASSERT_EQUAL_UINT32(1200, coolant.em - load.em);
  TEST_ASSERT_EQUAL(EstadoObd::lendo, bancada.maquina.estado());
}

static void teste_tempo_limite_na_descoberta_pede_o_mesmo_mapa() {
  Bancada bancada;
  bancada.responde = false;
  bancada.ligar();
  bancada.passar(1210);
  TEST_ASSERT_EQUAL_UINT(2, bancada.saida.pedidos_feitos);
  conferir_pedido(bancada.saida.pedidos[1], 0x7DF, 0x00);
  TEST_ASSERT_EQUAL_UINT32(1200, bancada.saida.pedidos[1].em - bancada.saida.pedidos[0].em);
  TEST_ASSERT_EQUAL(EstadoObd::descobrindo, bancada.maquina.estado());
}

static void teste_contador_de_ms_dando_a_volta() {
  // Os ms desde o boot dão a volta em ~49 dias: a agenda e o tempo limite seguem.
  Bancada bancada(0xFFFFF000UL);
  bancada.ligar();
  bancada.descobrir();
  const size_t primeiro = bancada.saida.pedidos_feitos;
  bancada.passar(8000);
  TEST_ASSERT_UINT_WITHIN(1, 100, bancada.pedidos_desde(primeiro));
  TEST_ASSERT_UINT_WITHIN(1, 100, bancada.saida.leituras_feitas);
}

static void teste_valor_escalado() {
  TEST_ASSERT_EQUAL_INT64(87600, valor_escalado(876.0f, 2));
  TEST_ASSERT_EQUAL_INT64(522, valor_escalado(133 * 100.0f / 255, 1));
  TEST_ASSERT_EQUAL_INT64(149, valor_escalado(38 * 100.0f / 255, 1));
  TEST_ASSERT_EQUAL_INT64(13800, valor_escalado(13.8f, 3));
  TEST_ASSERT_EQUAL_INT64(-40, valor_escalado(-40.0f, 0));
  TEST_ASSERT_EQUAL_INT64(1638375, valor_escalado(16383.75f, 2));  // o maior rpm
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(teste_parado_antes_de_comecar);
  RUN_TEST(teste_escuta_sem_trafego);
  RUN_TEST(teste_trafego_de_mais_de_2_s_nao_comeca);
  RUN_TEST(teste_trafego_comeca_a_descoberta_pelo_mapa_00);
  RUN_TEST(teste_descoberta_com_os_mapas_do_simulador);
  RUN_TEST(teste_mapa_sem_o_seguinte_termina_a_descoberta);
  RUN_TEST(teste_le_os_8_em_rodizio_com_os_valores_da_tabela);
  RUN_TEST(teste_pid_nao_suportado_fica_de_fora);
  RUN_TEST(teste_taxa_padrao_um_a_cada_80_ms);
  RUN_TEST(teste_taxa_de_15_pela_agenda_sai_exata);
  RUN_TEST(teste_taxa_de_5);
  RUN_TEST(teste_taxa_de_20);
  RUN_TEST(teste_central_lenta_sem_rajada);
  RUN_TEST(teste_nova_taxa_vale_no_proximo_pedido);
  RUN_TEST(teste_quadro_que_nao_e_a_resposta_e_ignorado);
  RUN_TEST(teste_resposta_sem_pedido_e_ignorada);
  RUN_TEST(teste_tempo_limite_passa_ao_proximo_pid);
  RUN_TEST(teste_tempo_limite_na_descoberta_pede_o_mesmo_mapa);
  RUN_TEST(teste_contador_de_ms_dando_a_volta);
  RUN_TEST(teste_valor_escalado);
  return UNITY_END();
}
