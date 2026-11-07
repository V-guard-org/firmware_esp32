# firmware_esp32

Firmware do ESP32 do V-GUARD (Oficina de Integração 2, UTFPR). O ESP32 lê o OBD-II do carro pelo MCP2515 e o acelerômetro ADXL345, grava um log no microSD e manda os dados à Raspberry Pi pela USB, em linhas de texto.

A especificação está na **`proposta-de-desenvolvimento.md`** da equipe, fora deste repositório: §4 (firmware) e §5 (serial ESP32 ↔ Pi). Este README não copia o conteúdo dela, para não existirem duas versões.

## Pinos

Placa: ESP32 clássico de 30 pinos, do tipo DevKit V1 (`board = esp32dev`).

| Peça | Barramento | Pinos |
|---|---|---|
| MCP2515 + TJA1050 | 1º SPI (VSPI) | SCK 18, MISO 19, MOSI 23, CS 25 |
| microSD | 2º SPI (HSPI) | SCK 27, MISO 32, MOSI 33, CS 26 |
| ADXL345 | I²C, endereço 0x53 | SDA 21, SCL 22 |

O simulador usa os mesmos pinos do MCP2515. No código, os pinos estão em `src/firmware/config.h`.

## Compilar, gravar e testar

Precisa do [PlatformIO](https://platformio.org/), pela extensão do VS Code ou pela linha de comando. Ele baixa a plataforma e as bibliotecas nas versões fixas do `platformio.ini`; o primeiro build leva alguns minutos.

| Ambiente | O que é | Comando |
|---|---|---|
| `esp32` | O firmware | `pio run -e esp32 -t upload` |
| `simulador` | A ECU simulada, num 2º ESP32 com outro MCP2515 | `pio run -e simulador -t upload` |
| `native` | Os testes do núcleo, no notebook | `pio test -e native` |

Para conferir as regras do núcleo (`CODING_STANDARDS.md`): `python ferramentas/checa_nucleo.py`.

## Ferramenta de bancada

`ferramentas/bancada.py` lê a USB do ESP32 no notebook, mostra as linhas e confere o checksum e a `seq` de cada uma. Ao sair, mostra o resumo: linhas válidas, corrompidas e perdidas.

| Comando | O que faz |
|---|---|
| `uv run ferramentas/bancada.py` | Mostra as linhas até o Ctrl+C |
| `uv run ferramentas/bancada.py --segundos 300` | Captura 5 min, mostra só os problemas e o resumo, e sai com erro se houver linha corrompida ou lacuna na `seq` |
| `uv run ferramentas/bancada.py --reiniciar` | Reinicia o ESP32 pelo EN ao abrir a porta, para pegar o `$B` |

Sem `--porta`, ela acha a placa pelo chip USB (CP210x ou CH340/CH9102). O `uv` instala o pyserial sozinho; sem ele, `pip install pyserial==3.5` e `python ferramentas/bancada.py`. Os testes dela: `python -m unittest discover -s ferramentas`.

Para só ver a serial, sem conferir nada: `pio device monitor` (115200 baud).

O trabalho está nas [Issues](https://github.com/V-guard-org/firmware_esp32/issues), e o jeito de trabalhar nelas está no `CLAUDE.md`.
