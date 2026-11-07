# firmware_esp32 (V-GUARD)

Firmware do ESP32 do V-GUARD. Lê o OBD-II pelo MCP2515 e o acelerômetro ADXL345, grava no cartão SD e manda linhas pela serial para a Raspberry Pi.

## Onde está a verdade

A especificação e as decisões estão na `proposta-de-desenvolvimento.md`, que fica fora deste repositório, na pasta acima do clone (`../proposta-de-desenvolvimento.md`). Quem não tiver a proposta pede ao Thiago. Se algo aqui discordar dela, vale a proposta.

Seções principais: §4 (firmware), §4.8 (organização deste repositório), §5 (serial ESP32 ↔ Pi) e §9 (roteiro de testes).

## Regras do código

Estão no `CODING_STANDARDS.md`. Leia antes de escrever ou revisar código.

## Fluxo por ticket

1. Rode `git fetch` e confira o `git status`. Crie um ramo novo a partir do `origin/main`.
2. Implemente com `/implement #N`, com os testes no núcleo (`pio test -e native`) e as regras dele (`python ferramentas/checa_nucleo.py`).
3. Cada mensagem de commit cita o ticket (`#N`).
4. Abra o PR com `Closes #N`. O Thiago revisa e faz o merge.

## Agent skills

### Issue tracker

Issues do GitHub de `V-guard-org/firmware_esp32`, pelo `gh`. Veja `docs/agents/issue-tracker.md`.

### Triage labels

Os cinco rótulos padrão: `needs-triage`, `needs-info`, `ready-for-agent`, `ready-for-human` e `wontfix`. Veja `docs/agents/triage-labels.md`.

### Domain docs

Um contexto só. As decisões ficam na proposta, sem `GLOSSARY.md` nem `docs/adr/`. Veja `docs/agents/domain.md`.
