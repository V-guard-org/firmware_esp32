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

No código, os pinos estão em `src/firmware/config.h`.

## Compilar, gravar e testar

Precisa do [PlatformIO](https://platformio.org/), pela extensão do VS Code ou pela linha de comando. Ele baixa a plataforma e as bibliotecas nas versões fixas do `platformio.ini`; o primeiro build leva alguns minutos.

| Ambiente | O que é | Comando |
|---|---|---|
| `esp32` | O firmware | `pio run -e esp32 -t upload` |
| `esp32-sim` | O firmware com a central simulada no lugar do MCP2515 (abaixo) | `pio run -e esp32-sim -t upload` |
| `native` | Os testes do núcleo, no notebook | `pio test -e native` |

Para conferir as regras do núcleo (`CODING_STANDARDS.md`): `python ferramentas/checa_nucleo.py`.

O `esp32-sim` é o mesmo firmware com uma central simulada no lugar do MCP2515, para a bancada sem o carro. A máquina do OBD é a mesma: descobre os PIDs (`$P` com `183A8001,00000001,40000000`) e pede os 8 em rodízio. Os valores seguem uma viagem que se repete a cada 5 min: parado em marcha lenta, acelera até 80 km/h, cruzeiro, freia e para. A temperatura do motor sobe de 47 °C a 90 °C nos primeiros 10 min. A versão termina em `-sim` no `$B` e no cabeçalho do SD, para os dados simulados não se confundirem com os do carro. Não precisa do MCP2515 ligado.

## Ferramenta de bancada

`ferramentas/bancada.py` lê a USB do ESP32 no notebook, mostra as linhas e confere o checksum, a `seq` e o `t` de cada uma. Ela também faz o papel da Pi: manda o `$H` com a hora do notebook ao ver um `$B` ou um `$S` com `ESPERA`, e depois a cada 60 s. Ao sair, mostra o resumo: linhas válidas, corrompidas e perdidas, a maior distância entre o `t` e a hora do notebook, o menor e o maior número de `$A` entre dois `$S`, e as médias de `ax`, `ay`, `az` e |a|.

| Comando | O que faz |
|---|---|
| `uv run ferramentas/bancada.py` | Mostra as linhas até o Ctrl+C |
| `uv run ferramentas/bancada.py --segundos 300` | Captura 5 min e mostra só os problemas, o que foi mandado e o resumo. Sai com erro se houver linha corrompida, lacuna na `seq`, `t` a mais de 1 s do notebook, `ESPERA` mais de 3 s depois do `$H`, fora de 49 a 51 `$A` entre dois `$S` em `ATIVO` ou `descartadas` acima de 0 |
| `uv run ferramentas/bancada.py --reiniciar` | Reinicia o ESP32 pelo EN ao abrir a porta, para pegar o `$B` |
| `uv run ferramentas/bancada.py --enviar C,obd_hz,15` | Manda a linha com a moldura (`$C,obd_hz,15*63`). Pode repetir; as linhas saem com 2 s de intervalo |
| `uv run ferramentas/bancada.py --sem-hora` | Não manda o `$H`, para ver o ESP32 em `ESPERA` |
| `uv run ferramentas/bancada.py --sem-imu` | Não confere a taxa do `$A`, para a bancada sem o ADXL345 ligado |

Com a ferramenta aberta, o que se digita no terminal, seguido de Enter, também vai ao ESP32 com a moldura: `C,obd_hz,15`. A linha que começa com `$` vai como foi digitada, para testar um checksum errado.

Sem `--porta`, ela acha a placa pelo chip USB (CP210x ou CH340/CH9102). O `uv` instala o pyserial sozinho; sem ele, `pip install pyserial==3.5` e `python ferramentas/bancada.py`. Os testes dela: `python -m unittest discover -s ferramentas`.

Para só ver a serial, sem conferir nada: `pio device monitor` (115200 baud).

O trabalho está nas [Issues](https://github.com/V-guard-org/firmware_esp32/issues), e o jeito de trabalhar nelas está no `CLAUDE.md`.
