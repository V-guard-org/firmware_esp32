# Validação — 0.10.1, fallback de 120 segundos — 27/11/2026

- Compilação concluída: Arduino CLI, placa `esp32:esp32:esp32doit-devkit-v1`, Arduino-ESP32 **3.3.12**.
- Flash: **357982 bytes / 1310720 bytes (27%)**.
- RAM estática: **31624 bytes / 327680 bytes (9%)**. Esse número não inclui as alocações das tarefas e buffers realizadas durante o setup; não é medição da folga de RAM em execução.
- Os **53 testes Python** das ferramentas originais de bancada e comparação do SD passaram, incluindo na cópia entregue em `data/ferramentas`.
- **11 cenários C++ executados e aprovados**, usando os fontes reais de relógio, protocolo e arquivo SD: limite de 120 s, hora anterior ao limite, sincronização tardia/repetida, hora inválida, espera sem arquivo, gravação autônoma, transição preservando dados/ordem, fluxo sincronizado anterior, falhas de abertura/escrita, falha na transição e relógio de 64 bits/reboot. Fonte em `data/testes/teste_fallback.cpp`; compilado no computador com Zig C++ 0.13.0, C++17 e `-Wall -Wextra -Werror`.
- `protocolo.cpp`, `obd.h/.cpp` e `imu.h/.cpp` continuam idênticos byte a byte ao projeto fornecido. `protocolo.h` recebeu o estado autônomo do relógio; `arquivo_sd.h/.cpp` receberam o arquivo sem hora e a transição para UTC.
- As implementações de CAN, ADXL e aquisição continuam equivalentes às originais, normalizando somente os includes. `saida.cpp` ganhou a verificação do prazo e a escolha de um nome livre para o arquivo autônomo.
- Taxas, limites, buffers, pilhas existentes e prioridades conferidos contra `config.h` original. A tarefa de status foi retirada do loop Arduino e criada explicitamente no núcleo 0.
- MCP_CAN 1.5.1 incluído com a licença original; única alteração na biblioteca: valor padrão de `DEBUG_MODE` de 1 para 0.

Comando de compilação, a partir da pasta que contém o sketch:

```text
arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 --warnings default can_sd_adxl_serial_rtos
```

A execução inicial do compilador foi bloqueada pelo ambiente restrito do Windows com `Illegal System DLL Relocation`. A execução fora dessa restrição terminou com sucesso. Não foi necessário mudar o firmware para contornar esse erro.

Não foi executada a suíte original C++ do PlatformIO. Os 11 cenários novos foram executados no computador, com um cartão simulado que captura arquivos e permite provocar falhas. Os 53 testes adicionais são das ferramentas Python. Nenhum deles é uma medição das tarefas FreeRTOS na ESP32.

O usuário confirmou o funcionamento da revisão anterior após enviar `$H`. Esta revisão de fallback ainda não foi gravada/testada fisicamente por nós. Teste sugerido: ligar sem enviar comandos, observar `ESPERA` até 120 s e depois `ATIVO`; conferir o arquivo `SEMxxxxx.csv`; enviar `$H` válido e conferir um arquivo UTC separado. A causa da intermitência específica do Voyage continua sem confirmação.
