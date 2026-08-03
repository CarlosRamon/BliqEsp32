# BliqEsp32 — Terminal BLE para Posto de Lavação / Aspiração

Firmware Arduino para ESP32 (placa **ESP32_CLP**) que se comunica via BLE com o app **BliqTotem (POS)**. Controla até 6 equipamentos de lavagem e aspiração por meio de um expansor I/O **TCA9535** (I2C), com suporte a múltiplos relés por máquina, pausa/retomada de sessão e heartbeat de sincronização com o totem.

---

## Hardware necessário

| Item | Detalhe |
|---|---|
| Placa | **ESP32_CLP** (ESP32 + TCA9535 via I2C no endereço `0x20`) |
| LED de status | GPIO **4** |
| Botão (BTN) | GPIO **15** |
| Interrupção (INT) | GPIO **5** |
| Relés | Port 1 do TCA9535 (8 saídas, RELAY_1 a RELAY_8) |
| Entradas digitais | Port 0 do TCA9535 (8 entradas com lógica invertida) |

Os relés são controlados via I2C — **não há pinos GPIO diretos** para as máquinas. A biblioteca `ESP32_CLP` abstrai toda a comunicação com o TCA9535.

---

## Dependências

- Arduino IDE (recomendado) ou PlatformIO
- Board Manager URL (ESP32):
  ```
  https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
  ```
- Versão do core: **ESP32 Arduino Core 3.x**
- Bibliotecas:
  - **ArduinoJson** by Benoit Blanchon — versão 6.x
  - **ESP32_CLP** — biblioteca local (instalar manualmente em `~/Documents/Arduino/libraries/`)

---

## Identificação BLE

O identificador único de cada unidade é o `BLE_SUFFIX` (12 hex chars), que deve ser cadastrado no backend como `uuidEsp32`.

| Item | Valor (exemplo) |
|---|---|
| `BLE_SUFFIX` | `89b28c48e335` |
| Nome do dispositivo | `CLP_89b28c48e335` |
| Service UUID | `00535500-0000-0000-0000-89b28c48e335` |
| Characteristic UUID | `00435500-0000-0000-0000-89b28c48e335` |

Para personalizar uma unidade, altere apenas a constante `BLE_SUFFIX` no topo do `.ino`. Os UUIDs e o nome são gerados automaticamente a partir dela.

> Os UUIDs devem ser **idênticos** no firmware e no app BliqTotem, senão a conexão BLE não funciona.

---

## Máquinas e mapeamento de relés

Cada máquina pode acionar **um ou mais relés** simultaneamente.

| Máquina | Relés ativados |
|---|---|
| `ASPIRADOR` | RELAY_1 |
| `AR_COMPRIMIDO` | RELAY_2 |
| `PERFUME` | RELAY_3 |
| `PRE_LAVAGEM` | RELAY_1 + RELAY_6 |
| `SHAMPOO` | RELAY_2 + RELAY_3 + RELAY_4 |
| `ENXAGUE` | RELAY_1 + RELAY_5 + RELAY_7 |

Apenas uma máquina fica ativa por vez — ao selecionar uma nova, todos os relés são desligados antes de ligar os da nova máquina.

---

## Comunicação BLE

O ESP32 expõe um único serviço com uma characteristic de 3 propriedades:

| Propriedade | Direção |
|---|---|
| `WRITE` | App → ESP32 (comandos) |
| `NOTIFY` | ESP32 → App (respostas e status) |
| `READ` | App lê o último valor publicado |

---

## Protocolo de comandos

### App → ESP32 (Write, JSON)

| Comando | Payload | Comportamento |
|---|---|---|
| `START` | `{ "action": "START", "duration": <segundos> }` | Inicia sessão; se sessão já ativa, sincroniza o timer (heartbeat) |
| `SELECT` | `{ "action": "SELECT", "machine": "<NOME>" }` | Ativa a máquina especificada (sem pausar o timer) |
| `PAUSE` | `{ "action": "PAUSE" }` | Desliga todas as máquinas e pausa o timer |
| `RESUME` | `{ "action": "RESUME" }` | Reativa a última máquina e retoma o timer |
| `STOP` | `{ "action": "STOP" }` | Desliga tudo e encerra a sessão imediatamente |
| `STATUS` | `{ "action": "STATUS" }` | Solicita o status atual da sessão |

> `duration` é em **segundos** (ex.: 600 = 10 minutos).

#### Heartbeat / sincronização

Se `START` for enviado enquanto uma sessão já está ativa, o ESP32 **não reinicia a sessão** — ele apenas atualiza o tempo restante com o valor enviado pelo totem. Se a sessão estava pausada (ex.: por queda BLE), ela é retomada automaticamente. Isso garante que o totem seja a fonte da verdade do tempo.

### ESP32 → App (Notify, JSON)

```json
// Sucesso — enviado após cada comando
{ "status": "OK", "machine": "SHAMPOO", "remaining": 243, "paused": false }

// Status atual — resposta ao comando STATUS
{ "status": "STATUS", "active": true, "remaining": 243, "machine": "SHAMPOO", "paused": false }

// Sessão encerrada (por STOP ou timer zerado)
{ "status": "DONE" }

// Erro
{ "status": "ERROR", "message": "sessao nao ativa" }
```

---

## Estado interno

| Variável | Tipo | Descrição |
|---|---|---|
| `sessionActive` | `bool` | Há uma sessão em andamento? |
| `activeMachine` | `int` | Índice da máquina ativa (-1 = nenhuma) |
| `isPaused` | `bool` | Sessão pausada? |
| `totalSeconds` | `int` | Duração total contratada em segundos |
| `sessionStart` | `unsigned long` | `millis()` no início da sessão |
| `elapsedPaused` | `unsigned long` | Total de milissegundos acumulados em pausa |

Tempo restante calculado dinamicamente:
```
restante = totalSeconds - (agora - sessionStart - elapsedPaused) / 1000
```

---

## Mecanismos de segurança

### Pausa automática por desconexão BLE
Se o BLE cair durante uma sessão ativa, o ESP32 **pausa** a sessão automaticamente (desliga todas as máquinas e congela o timer). Ao reconectar e receber um novo `START` (heartbeat), a sessão é retomada do ponto em que parou.

### Timer de segurança (fallback)
O ESP32 mantém seu próprio timer. Se o totem não enviar `STOP` a tempo e o timer zerar localmente:
1. Desliga todos os relés
2. Encerra a sessão internamente
3. Notifica `{ "status": "DONE" }` se ainda conectado

**O timer do totem é o principal** (é o que o cliente vê). O timer local é apenas proteção.

### Timeout de atividade BLE
Se nenhum comando BLE for recebido por **60 segundos** durante uma sessão ativa, a sessão é encerrada por segurança.

### Watchdog Timer (hardware)
O ESP32 usa `esp_task_wdt` com timeout de **10 segundos**. Se o `loop()` travar por qualquer motivo, o hardware reinicia o microcontrolador automaticamente.

---

## LED de status (GPIO 4)

| Estado | LED |
|---|---|
| Desconectado | Apagado |
| Conectado, sem sessão | Aceso fixo |
| Sessão pausada | Pisca lento (1 s) |
| Sessão em andamento | Pisca rápido (250 ms) |

---

## Re-advertising BLE

- **Imediato**: ao detectar desconexão, o advertising é reiniciado após 500 ms
- **Periódico**: a cada **30 segundos** enquanto desconectado (keepalive)

---

## Estrutura do código

```
BliqEsp32_Arduino.ino
├── setup()
│   ├── CLP.begin()         — inicializa GPIO, I2C, TCA9535 e relés em OFF
│   ├── Watchdog Timer      — timeout de 10 s para reset de hardware
│   └── BLE                 — nome, service, characteristic, advertising
│
└── loop()
    ├── esp_task_wdt_reset() — reseta o watchdog a cada ciclo
    ├── Re-advertising       — imediato ou periódico (30 s) se desconectado
    ├── LED de status        — reflete o estado atual da sessão
    ├── Timeout BLE (60 s)   — encerra sessão se não receber comandos
    ├── Timer de segurança   — encerra sessão quando tempo zera
    └── Log periódico (10 s) — imprime estado no Serial Monitor

Callbacks BLE:
  MyServerCallbacks  → onConnect / onDisconnect
  MyCharCallbacks    → onWrite → cmdStart / cmdSelect / cmdPause
                                  cmdResume / cmdStop / cmdStatus
```

---

## Logs Serial (115200 baud)

Abra o Serial Monitor em **115200 baud** para acompanhar o estado em tempo real:

```
╔══════════════════════════════════════════╗
║  BliqEsp32 — CLP_89b28c48e335           ║
╚══════════════════════════════════════════╝

[BLE] Advertising iniciado — aguardando app...
[BLE] Nome:               CLP_89b28c48e335
[BLE] Service UUID:        00535500-0000-0000-0000-89b28c48e335
[BLE] Characteristic UUID: 00435500-0000-0000-0000-89b28c48e335

╔════════════════════════════════╗
║  [BLE] Dispositivo CONECTADO   ║
╚════════════════════════════════╝

[BLE] Recebido: {"action":"START","duration":600}

╔═══════════════════════════════════════╗
║  SESSÃO INICIADA — AGUARDANDO SELECT  ║
║  Tempo total: 10min 0s               ║
╚═══════════════════════════════════════╝

[BLE] Recebido: {"action":"SELECT","machine":"SHAMPOO"}
[CMD] SELECT → SHAMPOO

┌─────────────────────────────────────┐
│  Máquina ativa: SHAMPOO
│  Restante: 09:43
│  Status: EM ANDAMENTO
└─────────────────────────────────────┘
```

---

## Pontos de atenção

1. **`millis()` estoura em ~49 dias** — para uso contínuo de longo prazo, o cálculo de tempo restante pode precisar tratar overflow
2. **Um ESP32 por box** — cada unidade tem seu próprio `BLE_SUFFIX` e é identificada individualmente no backend
3. **Sem autenticação BLE** — qualquer dispositivo que conheça os UUIDs pode enviar comandos; para produção, considerar emparelhamento com PIN
4. **ESP32 Arduino Core 3.x** — o Watchdog Timer usa a API `esp_task_wdt_config_t`, incompatível com o Core 2.x
