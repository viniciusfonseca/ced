# Revisão de conformidade da implementação de extensões

Data: 15/11/2026.

Documento auditado: [arquitetura-extensoes.md](arquitetura-extensoes.md), SHA-256 `5cee7b0c6184d918b0274054d515aa6a695df3b16d5bedae90f37fc4834931c7`. A [spec de autoria](extension-authoring-spec.md) também foi consultada, conforme indicado pela arquitetura.

**Resultado da auditoria original: atendimento parcial.** O fluxo funcional das fases 0 e 1 existia e passava pelos testes, mas havia não conformidades em requisitos do próprio MVP. As fases 2–4 continuam pendentes por decisão explícita do documento; sua ausência não foi classificada como regressão.

## Atualização após as correções

Os itens R1–R6 abaixo foram corrigidos. Suas descrições e reproduções são mantidas como histórico da auditoria, não como descrição do código atual.

- R1: resposta RPC completa e reserva de fila precedem o commit; os retornos das alocações são verificados e as reservas são liberadas em caso de falha.
- R2: concessão/revogação centralizadas no gerenciador; revogação, perda de escopo em Save As/rename e fechamento invalidam o trabalho afetado e encerram a instância quando necessário.
- R3: campos opcionais desconhecidos são tolerados em RPC; campos obrigatórios/tipos/exclusões continuam validados e manifestos permanecem estritos.
- R4: erros de revisão incluem a versão atual, parâmetros inválidos têm o código adequado e o exemplo preserva código, motivo e dados dos erros do host.
- R5: a árvore JSON é validada contra números não finitos, inclusive em objetos/arrays aninhados.
- R6: notificações inválidas geram diagnóstico limitado por frequência; abuso encerra a instância sem responder às notificações.

Também foram centralizados os botões residuais de Markdown/terminal, acrescentados dados operacionais na listagem e registros de request ID/erro/commit, e aplicado um orçamento cooperativo de despacho de 2 ms com alternância entre instâncias. O orçamento não interrompe uma transação atômica em andamento; adia mensagens seguintes.

Novos testes permanentes: `extension_broker` (falha em cada alocação JSON da resposta, filas reais/reservas, erros, campos opcionais, abuso e alternância de frames) e `extension_example` (executável real preservando erros do host e recusando snapshot obsoleto). A integração existente foi ampliada com revogação durante comando e Save As real para fora do workspace. O teste de lifecycle usa campos opcionais nas respostas da fixture.

Validação após as correções: **12/12 testes no build normal**, **9/9 testes de extensões/assistência com ASan/UBSan** e **4/4 no build sem extensões**. O broker também testa falha do commit depois da reserva da resposta e duas instâncias disputando a mesma revisão: somente a primeira confirma; a segunda recebe `StaleDocument` com a revisão atual. Os testes de filas usam as estruturas reais do transporte, em processo isolado sem worker, para tornar a injeção de falhas determinística.

As funcionalidades das fases 2–4, o watcher de perfil entre janelas e a validação de cotas por medições de referência continuam trabalho posterior. Não se declara conformidade integral com toda a API 1.0 proposta.

O documento e os arquivos novos de extensões ainda estão sem rastreamento no Git. Não há uma revisão anterior registrada de `arquitetura-extensoes.md` que permita determinar exatamente quais parágrafos foram alterados. Esta revisão compara a implementação com o conteúdo atual, sem atribuir os problemas a uma alteração específica do documento.

Não foram alterados código de produção, testes existentes ou o documento de arquitetura durante esta auditoria. Este relatório é o único arquivo acrescentado ao workspace nesta revisão.

## Não conformidades prioritárias

### R1 — P1: falha de alocação pode confirmar edição com resposta incompleta

Requisito: seção 9, passos 5–6, linhas 400–401. As alocações devem ser preparadas antes da mutação; falta de memória deve resultar em erro sem alterar o buffer.

Em [broker.c](../src/extensions/broker.c), linhas 123–128, somente o ponteiro retornado por `cJSON_CreateObject` é verificado. Os retornos de `cJSON_AddStringToObject`, `cJSON_AddNumberToObject` e `cJSON_AddBoolToObject` são ignorados. Se a alocação de um campo falhar, o objeto raiz continua válido e o commit acontece.

Reprodução isolada, injetando falha na quinta alocação cJSON desse pedido, após construir o pedido:

```text
OOM_REPLY commits=1 version=8 allocation=5
reply={"jsonrpc":"2.0","id":"ext:1","result":{"documentId":"doc-1","changed":true}}
```

O texto mudou de `abc` para `Xbc`, a versão passou de 7 para 8 e a resposta perdeu `version`. A mesma reprodução funcionou com ASan/UBSan. O defeito está no broker, antes do adaptador CUI; o teste existente de falha na alocação do checkpoint de undo não cobre esse caminho.

Correção necessária: validar a preparação completa dos dados da resposta e seus recursos antes do commit, além do checkpoint. Testar falhas de alocação em cada etapa do broker. A injeção usada nesta auditoria ocorre em processo de teste isolado e sem workers; não justifica alternar hooks globais de cJSON por pedido em produção.

### R2 — P2: revogação de documento não encerra a instância

Requisito: seção 13, linhas 545–546. Perda/revogação de acesso precisa invalidar operações incompatíveis; a revogação encerra/recria a instância.

Em [extension_ui.c](../src/extensions/extension_ui.c), linhas 58–59, **Revoke Active Document Access** somente remove o ID de `document_grants`. Não chama `ced_extensions_stop`, não muda a sessão e não cancela o comando em andamento. O caminho de Save As em [main.c](../src/main.c), linhas 230–238, também não notifica o gerenciador sobre a mudança de escopo.

O broker reavalia o caminho e a concessão antes dos próximos acessos, portanto consultas/edições posteriores de um documento externo sem concessão são recusadas. A divergência é a ausência do encerramento/cancelamento exigido, não uma afirmação de que a API passa a permitir acesso indevido. Eventos sintéticos dependem da fase 2, mas invalidar a instância não depende da implementação de eventos.

Correção necessária: centralizar alterações de concessão e escopo no gerenciador, invalidar o trabalho afetado e encerrar a instância; reativação posterior deve ser explícita/sob demanda.

### R3 — P2: campos opcionais desconhecidos quebram a conexão

Requisito: seção 8, linha 347; spec de autoria, seção 5.2. Campos opcionais desconhecidos de mensagens/respostas devem ser ignorados. A regra estrita de campos desconhecidos pertence ao manifesto.

[broker.c](../src/extensions/broker.c), linhas 139–144, e [manager.c](../src/extensions/manager.c), linhas 141–145, aplicam `ced_ext_json_fields` com uma lista fechada ao envelope RPC. Um campo adicional sem efeito operacional faz a instância falhar.

Reproduções com `futureOptional` no envelope:

```text
OPTIONAL_REQUEST_FIELD state=5 reply={"error":{"code":-32600,...},"jsonrpc":"2.0","id":null}
OPTIONAL_RESPONSE_FIELD state=5 (FAILED=5)
```

Correção necessária: validar os campos conhecidos obrigatórios e os tipos/exclusões do envelope sem rejeitar extensões opcionais; preservar a validação estrita de manifestos.

### R4 — P2: erros de revisão e parâmetros não cumprem o contrato

Requisitos: seção 9, linha 441, exige `error.data.currentVersion` para `StaleDocument`; seção 8, linha 329, exige `-32602` para parâmetros inválidos.

O helper de erro em [json.c](../src/extensions/json.c), linhas 106–114, só acrescenta `data.reason`, e o caminho de erro do broker não inclui a revisão atual:

```json
{"error":{"code":-32002,"message":"StaleDocument","data":{"reason":"StaleDocument"}},"jsonrpc":"2.0","id":"ext:1"}
```

Já `documents/get` sem `documentId` chega ao caso de documento não encontrado em [broker.c](../src/extensions/broker.c), linhas 187–192, retornando `-32011 / Unavailable` em vez de `-32602`.

Há também uma divergência no exemplo: [main.c da extensão](../examples/extensions/uppercase-selection/main.c), linhas 148–152, converte falhas do host em `-32004 / Cancelled`, mesmo quando o erro real é `StaleDocument`, `PermissionDenied` ou outro motivo. A mensagem pode conservar o texto original, mas o motivo programático é perdido.

Correção necessária: validar parâmetros antes de procurar documentos, incluir os dados de revisão exigidos e preservar os códigos/motivos adequados no exemplo.

### R5 — P2: números não finitos são aceitos no DOM

Requisito: validar tipos/valores da API e das mensagens; a spec de autoria, seção 5.2, proíbe valores não finitos.

O preflight de [json.c](../src/extensions/json.c), linhas 41–69, valida a sintaxe do número, mas a árvore retornada pelo cJSON não é percorrida para verificar finitude. `ced_ext_json_parse("{\"value\":1e999}", ...)` retorna um objeto com `valuedouble` infinito.

```text
NONFINITE_JSON accepted=yes
```

`ced_ext_json_uint` e o parser de configuração já verificam finitude em seus campos específicos, mas isso não estabelece a garantia para o restante do envelope/resultado JSON.

Correção necessária: validar finitude em todos os nós numéricos do DOM antes de despachar/aceitar a mensagem, com testes de expoentes que ultrapassem a representação do host.

### R6 — P2: erros em notificações são descartados sem diagnóstico

Requisito: seção 8, linha 330: violações em notificações são registradas, e repetição abusiva encerra a instância.

Em [broker.c](../src/extensions/broker.c), linha 164, notificações diferentes de `log/write` e `ced/cancel` retornam antes da identificação do método ou do registro do erro. `fail` também não registra nada quando `id` é ausente. A ausência de resposta está correta, mas não substitui o diagnóstico exigido.

```text
UNKNOWN_NOTIFICATION replies=0 logged=no
```

Correção necessária: registrar violações com uma política limitada de frequência e acompanhar abuso, mantendo notificações sem resposta.

## Atendimento por área e trabalho posterior

| Área | Situação na versão atual |
| --- | --- |
| Separação de processos e API própria | Implementada; sem ponteiros CUI no protocolo nem runtime obrigatório. |
| Instalação local e revisão | Implementadas: validação, SHA-256, cópia/registro atômico, concessões no perfil, retenção de versões e leases contra desinstalação em uso. |
| Confiança e filtro de documentos | Implementados, com a lacuna de revogação/ciclo de vida descrita em R2. |
| Ativação por comando | Implementada, com handshake, limites de processos, prazos, falha e reinício explícito. |
| Identidade/revisão e transações | Implementadas: UTF-8, offsets, revisão/seleção obsoletas, no-op, múltiplos ranges, um undo, redo e edição inativa. A garantia global de falta de memória depende de R1. |
| Registro de comandos | Paleta dinâmica, IDs e despacho dos principais atalhos/botões existem. Alguns botões ainda chamam helpers diretamente, como Markdown em `main.c:795` e Hide Terminal em `main.c:874`; a centralização descrita na seção 2 não está completa. |
| Parser e I/O em workers | Implementados, com filas e cotas. Há divergências de contrato em R3–R6. |
| Configuração, estado e eventos | Não implementados como serviços; não são anunciados em `hostMethods`. A fase 2 permanece pendente. Esquemas de configuração são aceitos, mas os valores ainda não são consumidos. |
| Atalhos/menus contribuídos | Não implementados. O manifesto rejeita esses pontos; o exemplo amplo da seção 5 da arquitetura não pode ser instalado no host atual sem remover suas contribuições futuras. |
| Views, temas, diagnósticos e servidores LSP de extensões | Fases 3–4 ainda pendentes, conforme o documento. O clangd interno é preservado. |
| Modo desabilitado | Existe em build (`CED_ENABLE_EXTENSIONS=OFF`) e execução (`--disable-extensions`). |

Há também diferenças operacionais já declaradas em [extensions.md](extensions.md), que precisam continuar separadas de uma alegação de conformidade integral:

- A arquitetura propõe frames de 16 MiB, filas de 16 MiB/64 pedidos e orçamentos separados para transporte e estruturas parseadas. O host negocia frames de 8 MiB, filas de oito mensagens/8 MiB e compartilha um orçamento de 64 MiB. Esses valores são negociados e documentados; os defaults da arquitetura são propostas, não resultados medidos. Faltam as medições de referência pedidas pela seção 17 para justificar o ajuste.
- O despacho limita quatro mensagens por instância, mas não implementa um orçamento temporal agregado de 2 ms nem rotaciona o início da passagem entre instâncias. Não foi medido desempenho de frames nesta auditoria; não há evidência de atendimento à meta temporal.
- A listagem atual mostra nome, ID, versão, estado e diagnósticos, mas não apresenta todos os dados da seção 16: origem, capacidades, tempo de ativação, requisições pendentes e bytes nas filas.
- Os logs registram método e duração, com identidade/sessão implícitas no caminho. Não há registro completo de request ID, erro e commit confirmado como exigido pelas seções 7 e 16. A recuperação e o diagnóstico de resultado desconhecido ficam incompletos.
- Não existe atualização automática do catálogo/confiança entre janelas; o perfil implementado declara essa limitação.

## Verificações executadas e limites da cobertura

- Build atual com extensões e CTest completo: **10/10 passaram**.
- CTest do build existente sem extensões: **4/4 passaram**.
- Probe de conformidade isolado compilado contra as bibliotecas atuais: reproduziu R1, R3, os dois erros do broker em R4, R5 e R6.
- O mesmo probe foi recompilado contra as bibliotecas ASan/UBSan e repetiu os resultados sem diagnóstico desses sanitizadores. Isso confirma comportamento funcional incorreto; ausência de erro de memória detectado não comprova conformidade.
- R2 e as observações de operação/observabilidade foram verificadas por leitura dos caminhos de código, sem alegar teste de encerramento real por clique.
- Esta revisão local ocorreu no macOS. A matriz de CI configura Ubuntu/macOS com extensões ON/OFF, mas os jobs remotos não foram executados nesta auditoria.

As suítes atuais cobrem o fluxo principal, mas não equivalem a todos os critérios da seção 17. Faltam testes específicos dos casos reproduzidos e de cenários como perda de acesso após Save As com operação em andamento, contexto forjado, disputa real entre duas extensões por um buffer, saturação das filas reais, escrita parcial efetivamente forçada e EOF no meio de um frame. Um helper retornar `Unavailable` ou uma reserva genérica falhar não comprova o comportamento de toda a integração nesses cenários.

Os arquivos temporários desta auditoria estão em `/tmp/ced-extension-audit-6ewUB0/`: `audit.c`, `results.txt`, `asan-results.txt`, `ctest-on.txt` e `ctest-off.txt`. O probe usa filas privadas em memória, não inicia extensões, não altera o perfil real e não modifica documentos do usuário. Esses arquivos são evidência local temporária, não uma adição à suíte permanente.

## Ordem recomendada para adequação

1. Corrigir R1 e acrescentar testes de falha de alocação no broker.
2. Corrigir o ciclo de vida de revogação/perda de escopo de R2.
3. Corrigir validação e erros RPC de R3–R6 e o encaminhamento de erros do exemplo.
4. Completar a centralização de comandos e a observabilidade já descrita; medir as cotas/metas do frame.
5. Implementar a fase 2 com seus testes antes de declarar API 1.0 estável; manter fases 3–4 como trabalho posterior.
