# Regras do código

As regras para escrever e revisar o código deste repositório. Vieram da proposta (§4.8) e agora ficam só aqui. Mudar uma regra é decisão do Thiago, feita neste arquivo por PR.

## Garantidas pela máquina

A configuração e a checagem automática (`checagem.yml`) garantem estas, e a revisão pode pulá-las:

- **No núcleo (`lib/nucleo/`), só memória de tamanho fixo e nada do hardware:** `ferramentas/checa_nucleo.py` recusa `new`, `malloc`, `String`, os contêineres da biblioteca padrão e os cabeçalhos do Arduino, do `mcp_can` e do FreeRTOS.
- **No `native`, `-Wall -Wextra -Werror`, sem exceções e sem RTTI:** o `platformio.ini` e o `native_cxx.py` ligam essas opções, e o `test/test_esqueleto` confere.
- **C++17 nos três ambientes:** os `static_assert` dos dois `main.cpp` e do teste.
- **Na parte do ESP32, avisos ligados, sem `-Werror`:** o `build_src_flags` do `platformio.ini`. O Arduino e o `mcp_can` geram avisos fora do nosso controle, por isso os avisos ficam só nas fontes do projeto.

## Para a revisão

### Língua

- Nomes de funções, variáveis, arquivos e pastas em português, sem acentos, como `lib/nucleo/`, `arquivo_sd` e os nomes do protocolo (`ESPERA`, `ATIVO`).
- Comentários e mensagens de erro em português, com acentos.
- As funções copiadas do Nicolas (`obdPositive`, `obdSupported` etc.) ganham nome em português na cópia, com a origem citada num comentário.

### Memória

- **No núcleo:** só buffers de tamanho fixo. O motivo: o `$A` sai 50 vezes por segundo, e pedir e devolver memória a cada linha picota a memória do ESP32 em algumas horas, até travar. A checagem pega as formas diretas; a revisão olha o resto, como uma função de biblioteca que pede memória por dentro.
- **Na parte do ESP32 (`src/`):** memória só no `setup()`, para as filas e as tarefas.
