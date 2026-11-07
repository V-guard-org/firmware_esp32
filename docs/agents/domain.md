# Documentos de domínio

Como as skills de engenharia leem as decisões deste repositório.

## Antes de explorar, leia

- **`../proposta-de-desenvolvimento.md`**, fora do repositório: é a especificação e o registro das decisões do V-GUARD. Para este repositório, importam mais a §4 (firmware), a §4.8 (organização e regras do código), a §5 (serial ESP32 ↔ Pi) e a §9 (roteiro de testes).

Se a proposta não estiver acessível, como num clone de outra pessoa, siga o ticket. Cada ticket traz os valores concretos de que precisa.

## Sem glossário nem ADRs

Este repositório não tem `GLOSSARY.md`, `GLOSSARY-MAP.md` nem `docs/adr/`, e não deve ganhar esses arquivos: a proposta faz esse papel. Quando uma skill, como `/domain-modeling` ou `/grill-with-docs`, quiser registrar um termo ou uma decisão, registre na proposta. Siga as regras do `CLAUDE.md` da pasta `Oficinas2/`:

- status Decidido, Proposto ou Em aberto;
- lista de itens em aberto da §10;
- linha datada no registro de alterações.

## Use o vocabulário da proposta

Ao nomear um conceito (título de ticket, nome de teste, proposta de refatoração), use o termo da proposta, como `núcleo`, `ATIVO`, `ESPERA`, `$A` e `$S`. Não invente sinônimos.

Se o conceito ainda não tem nome na proposta, é um sinal: ou o termo é inventado (reconsidere), ou falta uma decisão (aponte para o Thiago).

## Aponte conflitos com a proposta

Se o trabalho contradiz a proposta, diga isso claramente em vez de passar por cima:

> _Contradiz a §4.4 (offset aplicado no núcleo), mas vale reabrir porque…_

Quem decide é o Thiago. Até ele confirmar, vale a proposta.
