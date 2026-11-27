# CAN + ADXL345 + SD + serial para a Raspberry

Versão reorganizada do firmware fornecido pelo colega, com a pinagem do `can_sd_adxl_soldado`. Abra `can_sd_adxl_serial_rtos.ino` no Arduino IDE, mantendo **a pasta inteira**, inclusive `src`.

## Organização

| Arquivo | Onde mexer |
|---|---|
| `can_sd_adxl_serial_rtos.ino` | Inicialização, criação do status periódico e visão geral |
| `config.h` | Pinos, núcleos, taxas, tamanhos das filas e pilhas |
| `aquisicao.cpp` | MCP2515, ADXL345 e tarefa de aquisição |
| `saida.cpp` | Comunicação com a Pi, buffers e gravação no SD |
| `interfaces.h` | Declarações compartilhadas entre as abas |
| `src/nucleo/` | Protocolo, máquina OBD, tratamento do ADXL e formato do log originais |
| `src/mcp_can/` | MCP_CAN 1.5.1 incluído com mensagens de depuração desativadas |

Não é necessário PlatformIO nem instalar MCP_CAN separadamente. SPI, SD e FreeRTOS acompanham o pacote ESP32. A cópia local do MCP_CAN evita que mensagens como `Setting Filter Successful!` contaminem o protocolo USB. A biblioteca instalada no computador não é alterada.

## Divisão entre os núcleos

| Núcleo | Tarefas contínuas |
|---|---|
| **1** | Uma tarefa lê CAN e ADXL a cada 10 ms; interpreta respostas e coloca mensagens nos buffers |
| **0** | Tarefas de envio serial, recepção dos comandos da Pi, gravação SD e status de 1 Hz |

As tarefas de SD e serial são separadas, mas fixadas no **mesmo núcleo 0**. Assim, uma espera no cartão não obriga o envio serial a aguardar a mesma função. A escrita efetiva no cartão e na UART não ocorre na tarefa de aquisição. A formatação e cópia das mensagens para os buffers ainda ocorrem em quem produz cada mensagem, sob o mutex original.

O `setup()` inicializa os periféricos antes das tarefas de aquisição e gravação; o `loop()` fica suspenso. Filas continuam limitadas: atrasos prolongados podem provocar descartes, informados pelo protocolo original.

## Pinagem do módulo soldado

| Módulo | SCK/CLK | MISO | MOSI | CS |
|---|---:|---:|---:|---:|
| MCP2515 | 14 | 12 | 13 | 15 |
| microSD | 18 | 19 | 23 | 5 |

ADXL345: SDA **21**, SCL **22**, endereço **0x53**, CS em 3,3 V e SDO em GND. Os pinos foram copiados do sketch soldado, não do projeto PlatformIO do colega. São dois controladores SPI separados, mesmo com os pinos remapeados.

## O que permanece igual ao projeto do colega

- USB/serial em **115200 baud, 8N1**.
- Mensagens `$B`, `$S`, `$L`, `$A`, `$O`, `$P`; comandos `$H` e `$C,obd_hz,...`.
- Mesmos campos, checksum XOR, sequência, timestamps e tratamento de descartes.
- Com `$H` válido, inicia imediatamente. **Sem `$H` por 120 segundos desde o boot, inicia sozinho.** A conferência inicial do ADXL ocorre no boot.
- ADXL em ±16 g, leitura de 100 Hz e envio filtrado de **50 Hz**, em **m/s²**, incluindo gravidade. Não substituí pelo formato em g do logger antigo.
- OBD com **12,5 pedidos/s no total**, um pedido pendente por vez. A Pi pode ajustar entre 1 e 20 pedidos/s pelo comando existente. Isso não significa 12,5 Hz por PID.
- Os oito campos OBD originais: `load`, `coolant`, `map`, `rpm`, `speed`, `iat`, `throttle`, `voltage`, somente quando anunciados pela ECU. Mantidos descoberta, timeout, suspensão de PID e reconexão originais.
- CAN de 500 kbit/s, MCP2515 de 8 MHz, resposta 0x7E8 e solicitação física 0x7E0; descoberta inicial em 0x7DF.
- SD em FAT32, arquivos por hora UTC `AAAAMMDD-HH.csv`, com cabeçalho e linhas do protocolo sem a moldura. **Não é o CSV unificado do logger antigo.** Preservar isso mantém a compatibilidade das ferramentas do colega.
- Confirmação da gravação prevista a cada segundo; isso não garante perda máxima de um segundo em falhas de alimentação ou atrasos do cartão.

Mudanças funcionais intencionais: pinagem, fixação das tarefas da serial/status no núcleo 0, CS em nível alto antes da inicialização e fallback de 120 segundos. Versão `0.10.1-soldado-rtos`. O formato das mensagens, comandos, checksum, algoritmo OBD e tratamento da IMU permanecem iguais. Não há servidor web, GPS ou dados simulados nesta versão.

## Fallback após 120 segundos

O prazo está em `config.h`, constante `espera_hora_ms = 120000`. É contado desde o boot, não desde a última mensagem recebida. Mensagens inválidas não reiniciam o prazo. Se chegar um `$H` válido antes do prazo, o funcionamento sincronizado é igual ao anterior.

Sem a hora, ao atingir o prazo a ESP muda para `ATIVO`, tenta ler CAN, envia a aceleração e começa a gravar. Emite um `$L` com `Modo autonomo: t em ms desde boot`. Nesse modo, **o campo `t` das mensagens e do SD contém milissegundos desde o boot, não uma data Unix**. Essa é a diferença de significado do tempo em relação ao protocolo anterior; campos e checksum continuam iguais. Um consumidor na Pi não deve interpretar esse tempo relativo como data civil. A ferramenta `bancada.py` envia `$H` ao receber a primeira linha, mesmo que encontre a ESP já ativa.

No cartão, cria `SEM00001.csv`, depois `SEM00002.csv` etc., escolhendo um nome livre por sessão autônoma. Não cria um arquivo novo a cada hora enquanto estiver sem hora civil. O cabeçalho explica que `t` é relativo ao boot. Não grava os dados dos primeiros 120 segundos retroativamente; o boot guardado é incluído.

Se a Raspberry enviar `$H` depois, a ESP passa para timestamps Unix e abre o arquivo UTC correspondente. O arquivo sem hora é preservado e não é renomeado nem corrigido retroativamente. Não é necessário reiniciar. O fallback também permite aquisição se não houver SD; a gravação continua dependendo de o cartão funcionar.

## Abrir e testar

1. Extraia a pasta completa. Abra o `.ino` de mesmo nome.
2. No Arduino IDE, selecione **DOIT ESP32 DEVKIT V1**, pacote **esp32 da Espressif 3.3.12**, e a porta da sua placa. Deixe **Core Debug Level: None**.
3. Compile e grave. Depois, feche o Monitor Serial antes de a Raspberry abrir a mesma porta.
4. Confira `$B`, depois `$S`. Após a Pi enviar `$H`, ou após 120 s sem ele, o estado deve passar de `ESPERA` para `ATIVO`. `$A` depende do ADXL responder; `$P` e `$O` dependem de tráfego e respostas OBD.
5. Confira a gravação no cartão e os contadores de descartes durante um teste contínuo.

As ferramentas do colega estão em `data/ferramentas/`, preservadas. Suas verificações de data e nome de arquivo foram feitas para operação sincronizada; não use esses critérios para validar os arquivos `SEMxxxxx.csv` do fallback. Sem a Raspberry, `bancada.py` pode mandar a hora e conferir a comunicação, usando Python e pyserial 3.5:

```text
python data/ferramentas/bancada.py --porta COM6 --segundos 60
python -m unittest discover -s data/ferramentas
```

Troque COM6 pela porta real. A ferramenta abre a serial; não execute enquanto outro programa usa a mesma porta.

## Validação e limites

Veja `VALIDACAO.md` para os resultados desta entrega. Compilar e comparar os fontes não comprova funcionamento físico com a Pi, o carro, o SD ou o ADXL. Esta reorganização **não comprova correção das quedas de CAN observadas no Voyage**; o comportamento OBD foi preservado por solicitação.

A organização `src` segue a [especificação de sketches do Arduino](https://docs.arduino.cc/arduino-cli/sketch-specification/); a fixação por núcleo usa a [API FreeRTOS do ESP32](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/freertos_additions.html).
