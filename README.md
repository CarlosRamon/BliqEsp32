# BliqEsp32 — Terminal BLE multi-máquina para Posto de Lavagem

Firmware para ESP32 que se comunica via BLE com o app **BliqTeste (POS)**.  
Controla até 4 equipamentos de lavagem por relé, com suporte a troca de máquina, pausa e retomada de sessão.

---

## Visão geral

O ESP32 atua como um **servidor BLE** (Bluetooth Low Energy). Ele fica aguardando o app POS conectar, recebe comandos em JSON, controla as máquinas de lavagem através de pinos GPIO e envia respostas de volta ao app.

---

## Requisitos

- Arduino IDE (recomendado) ou PlatformIO
- Placa: **ESP32 Dev Module**
- Board Manager URL:
  ```
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  ```
- Bibliotecas (instalar pelo Library Manager):
  - **ArduinoJson** by Benoit Blanchon — versão 6.x

---

## Pinos

```
LED status  → GPIO 2   (onboard)
PRE_LAVAGEM → GPIO 26
ESPUMA      → GPIO 27
ENXAGUE     → GPIO 14
FINALIZAR   → GPIO 12
```

Cada pino de máquina controla um **relé externo**. Apenas uma máquina fica ativa por vez — o código desativa todos os pinos antes de ativar o novo.

> **Atenção:** os pinos assumem relé **ativo-alto** (HIGH = ligado). Se o seu módulo de relé for ativo-baixo, inverta `HIGH`/`LOW` nas funções `activateMachine` e `deactivateAll`.

---

## Comunicação BLE

O ESP32 expõe um único serviço BLE com uma única characteristic que tem 3 propriedades:

| Propriedade | Uso |
|---|---|
| `WRITE` | App → ESP32 (enviar comandos) |
| `NOTIFY` | ESP32 → App (respostas e notificações) |
| `READ` | App lê o último valor |

| Item | UUID |
|---|---|
| Nome do dispositivo | `ESP32_LAVAGEM` |
| Service | `12345678-1234-1234-1234-1234567890ab` |
| Characteristic | `abcd1234-5678-90ab-cdef-1234567890ab` |

> Os UUIDs precisam ser **idênticos** no firmware e no app, senão a conexão não funciona.

---

## Protocolo de comandos

### App → ESP32 (Write)

| Comando | Payload | O que faz |
|---|---|---|
| `START` | `{ "action": "START", "duration": <minutos> }` | Inicia sessão, ativa PRE_LAVAGEM, inicia timer interno |
| `SELECT` | `{ "action": "SELECT", "machine": "<NOME>" }` | Troca a máquina ativa (sem pausar o timer) |
| `PAUSE` | `{ "action": "PAUSE" }` | Desliga máquina atual, congela timer interno |
| `RESUME` | `{ "action": "RESUME" }` | Religa última máquina, retoma timer interno |
| `STOP` | `{ "action": "STOP" }` | Desliga tudo, encerra sessão |

O campo `machine` aceita: `PRE_LAVAGEM`, `ESPUMA`, `ENXAGUE` ou `FINALIZAR`.

### ESP32 → App (Notify)

```json
// Sucesso — enviado após cada comando
{ "status": "OK", "machine": "ESPUMA", "remaining": 243, "paused": false }

// Sessão encerrada (por STOP ou timer zerado)
{ "status": "DONE" }

// Erro
{ "status": "ERROR", "message": "sessao nao ativa" }
```

---

## Estado interno

O ESP32 mantém 6 variáveis de estado:

| Variável | Tipo | Descrição |
|---|---|---|
| `sessionActive` | `bool` | Há uma sessão em andamento? |
| `activeMachine` | `int` | Índice da máquina ativa (-1 = nenhuma) |
| `isPaused` | `bool` | Sessão pausada? |
| `totalMinutes` | `int` | Duração total contratada |
| `sessionStart` | `unsigned long` | `millis()` no início da sessão |
| `elapsedPaused` | `unsigned long` | Total de milissegundos em pausa |

O tempo restante é calculado dinamicamente:
```
restante = totalMinutes*60 - (agora - sessionStart - elapsedPaused) / 1000
```

---

## Timer interno (fallback de segurança)

O ESP32 tem seu próprio timer além do que o app controla. Ele existe para o caso do **Bluetooth cair no meio da sessão** — sem o fallback, as máquinas ficariam ligadas indefinidamente.

Quando o tempo zera no ESP32:
1. Desliga todos os pinos
2. Encerra a sessão internamente
3. Notifica `{ "status": "DONE" }` se ainda houver conexão BLE

**O timer do app é o principal** (é o que o cliente vê). O timer do ESP32 é só proteção.

---

## LED de status (GPIO 2)

| Estado | LED |
|---|---|
| Desconectado | Apagado |
| Conectado, sem sessão | Aceso fixo |
| Sessão pausada | Pisca lento (1s) |
| Sessão em andamento | Pisca rápido (250ms) |

---

## Estrutura do código

```
setup()
├── Configura pinos (LED + 4 relés)
├── Inicializa BLE (nome, service, characteristic)
└── Inicia advertising (fica visível para o app)

loop()
├── Re-advertising se houve desconexão
├── Controle do LED conforme estado
├── Verifica timer zerado (fallback de segurança)
└── Log periódico no Serial (a cada 10s)

MyServerCallbacks → onConnect / onDisconnect
MyCharCallbacks   → onWrite (ponto de entrada dos comandos)
  └── despacha para: cmdStart / cmdSelect / cmdPause / cmdResume / cmdStop
```

---

## Logs Serial (115200 baud)

Abra o Serial Monitor em **115200 baud** para acompanhar o estado em tempo real:

```
╔══════════════════════════════════════════╗
║  BliqEsp32 — Posto de Lavagem            ║
║  Multi-máquina BLE                       ║
╚══════════════════════════════════════════╝

[BLE] Advertising iniciado — aguardando app...

╔════════════════════════════════╗
║  [BLE] Dispositivo CONECTADO   ║
╚════════════════════════════════╝

[BLE] Recebido: {"action":"START","duration":10}

╔═══════════════════════════════════════╗
║      SESSÃO INICIADA — PRE_LAVAGEM    ║
║  Tempo total: 10 min
╚═══════════════════════════════════════╝

[BLE] Recebido: {"action":"SELECT","machine":"ESPUMA"}
[CMD] SELECT → ESPUMA

┌─────────────────────────────────────┐
│  Máquina ativa: ESPUMA
│  Restante: 07:43
│  Status: EM ANDAMENTO ...
└─────────────────────────────────────┘
```

---

## Pontos de atenção para melhorias

1. **`millis()` estoura em ~49 dias** — para uso em produção, o cálculo de tempo restante precisa tratar overflow
2. **Um ESP32 por box** — a arquitetura atual é 1 ESP32 por ponto de lavagem, cada um com seu próprio nome BLE
3. **Sem autenticação BLE** — qualquer dispositivo que conheça os UUIDs consegue enviar comandos; considerar emparelhamento com PIN
4. **Pinos assumem relé ativo-alto** — se o relé for ativo-baixo, inverter `HIGH`/`LOW` nas funções `activateMachine` e `deactivateAll`
5. **Serial Monitor** — durante desenvolvimento, abrir em 115200 baud para ver todo o log de estado e comandos
