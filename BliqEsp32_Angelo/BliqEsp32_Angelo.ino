// ============================================================
//  BliqEsp32_Angelo — Terminal BLE para Posto de Lavação / Aspiração
//  Placa: LIPXTECH "Angelo" — ESP32-S3 10-CHANNEL RELAY CONTROLLER
//  (relés acionados direto por GPIO via ULN2003 — SEM expander I2C)
//
//  Para a placa legada (CLP + TCA9535 via I2C) use ../BliqEsp32_Arduino/
//
//  Dependencias (instalar pelo Library Manager):
//    - ArduinoJson  (by Benoit Blanchon)  versao 6.x
//
//  Protocolo BLE — comandos recebidos (JSON):
//    { "action": "START",  "duration": <minutos> }  → inicia sessão
//    { "action": "SELECT", "machine": "<NOME>"   }  → ativa máquina
//    { "action": "PAUSE"                         }  → pausa máquina
//    { "action": "RESUME"                        }  → retoma máquina
//    { "action": "STOP"                          }  → encerra sessão
//
//  Nota: o totem é a fonte da verdade do tempo.
//  O ESP32 mantém um timer interno apenas como segurança
//  (desliga máquinas se BLE cair e STOP não chegar a tempo).
// ============================================================

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// ── UUIDs ────────────────────────────────────────────────────
// BLE_SUFFIX = 12 hex chars cadastrados no backend como uuidEsp32
#define BLE_SUFFIX "90e02542161b"
#define SERVICE_UUID        "00535500-0000-0000-0000-" BLE_SUFFIX
#define CHARACTERISTIC_UUID "00435500-0000-0000-0000-" BLE_SUFFIX

// ── Seleção de placa alvo ─────────────────────────────────────
// 0 = ESP32 clássico (debug/bancada) — pinos remapeados p/ não colidir
//     com o flash interno (GPIO 6-11) nem strapping (GPIO 12).
//     Nesta placa os relés só disparam se você ligar fios do ESP32
//     direto nos drivers, bypassando o TCA9535.
// 1 = Placa Angelo ESP32-S3 (produção) — pinos MCU_C1..MCU_C10.
#define BOARD_TIPO 0

#define DEVICE_NAME "CLP_" BLE_SUFFIX
#define MACHINE_COUNT 6
#define RELAY_COUNT 10

#if BOARD_TIPO == 0
  // ESP32 clássico — GPIOs seguros p/ output (evita flash 6-11, strapping 0/2/12/15, input-only 34-39)
  #define LED_PIN 2
  const uint8_t RELAY_PINS[RELAY_COUNT] = {
    4,   // RELAY_1
    5,   // RELAY_2
    13,  // RELAY_3
    14,  // RELAY_4
    16,  // RELAY_5
    17,  // RELAY_6
    18,  // RELAY_7
    19,  // RELAY_8  (reservado)
    21,  // RELAY_9  (reservado)
    22   // RELAY_10 (reservado)
  };
#elif BOARD_TIPO == 1
  // Placa Angelo ESP32-S3 — GPIOs conectados aos drivers ULN2003 dos relés
  #define LED_PIN 2
  const uint8_t RELAY_PINS[RELAY_COUNT] = {
    4,   // RELAY_1  → MCU_C1
    5,   // RELAY_2  → MCU_C2
    6,   // RELAY_3  → MCU_C3
    7,   // RELAY_4  → MCU_C4
    8,   // RELAY_5  → MCU_C5
    9,   // RELAY_6  → MCU_C6
    10,  // RELAY_7  → MCU_C7
    11,  // RELAY_8  → MCU_C8  (reservado)
    12,  // RELAY_9  → MCU_C9  (reservado)
    13   // RELAY_10 → MCU_C10 (reservado)
  };
#else
  #error "BOARD_TIPO invalido: use 0 (ESP32 classico) ou 1 (Angelo ESP32-S3)"
#endif

// Aliases para manter as máscaras legíveis (bit index de RELAY_PINS)
#define RELAY_1 0
#define RELAY_2 1
#define RELAY_3 2
#define RELAY_4 3
#define RELAY_5 4
#define RELAY_6 5
#define RELAY_7 6

const char* MACHINE_NAMES[MACHINE_COUNT] = { "ASPIRADOR", "AR_COMPRIMIDO", "PERFUME", "PRE_LAVAGEM", "SHAMPOO", "ENXAGUE" };
// Máscara de bits: bit N = relé (N+1) energizado
const uint16_t MACHINE_MASKS[MACHINE_COUNT] = {
  (1 << RELAY_1),                                    // Aspirador     → RL1
  (1 << RELAY_2),                                    // Sopro         → RL2
  (1 << RELAY_3),                                    // Perfume       → RL3
  (1 << RELAY_1) | (1 << RELAY_6),                   // PRE_LAVAGEM   → RL1 + RL6
  (1 << RELAY_2) | (1 << RELAY_3) | (1 << RELAY_4),  // SHAMPOO       → RL2 + RL3 + RL4
  (1 << RELAY_1) | (1 << RELAY_5) | (1 << RELAY_7)   // ENXAGUE       → RL1 + RL5 + RL7
};

// ── Estado global ─────────────────────────────────────────────
BLEServer* bleServer = nullptr;
BLECharacteristic* bleCharacteristic = nullptr;

bool deviceConnected = false;
bool wasConnected = false;

// Sessão
bool sessionActive = false;
int activeMachine = -1;
bool isPaused = false;
int totalSeconds = 0;
unsigned long sessionStart = 0;
unsigned long elapsedPaused = 0;
unsigned long pauseStart = 0;

unsigned long lastBleActivity = 0;
#define BLE_TIMEOUT_MS 60000

unsigned long lastLogTime = 0;
unsigned long lastAdvRestartMs = 0;
#define LOG_INTERVAL_MS 10000
#define ADV_RESTART_INTERVAL 30000

// ── Helpers de máquina ────────────────────────────────────────

// Aplica uma máscara de bits nas saídas: bit N HIGH → RELAY_PINS[N] HIGH.
// ULN2003 é ativo em HIGH (GPIO alto → transistor satura → bobina energiza).
static void writeRelayMask(uint16_t mask) {
  for (int i = 0; i < RELAY_COUNT; i++) {
    digitalWrite(RELAY_PINS[i], (mask & (1u << i)) ? HIGH : LOW);
  }
}

// Desliga todos os relés
void deactivateAll() {
  writeRelayMask(0);
}

// Liga apenas os relés correspondentes à máquina selecionada
void activateMachine(int idx) {
  if (idx >= 0 && idx < MACHINE_COUNT) {
    writeRelayMask(MACHINE_MASKS[idx]);
  } else {
    deactivateAll();
  }
}

int findMachine(const char* name) {
  for (int i = 0; i < MACHINE_COUNT; i++) {
    if (strcmp(MACHINE_NAMES[i], name) == 0) return i;
  }
  return -1;
}

unsigned long remainingSeconds() {
  if (!sessionActive) return 0;
  unsigned long totalMs = (unsigned long)totalSeconds * 1000UL;
  unsigned long now = millis();
  unsigned long elapsed;
  if (isPaused) {
    elapsed = (now - sessionStart) - elapsedPaused - (now - pauseStart);
  } else {
    elapsed = now - sessionStart - elapsedPaused;
  }
  if (elapsed >= totalMs) return 0;
  return (totalMs - elapsed) / 1000UL;
}

// ── Envio de notificação ──────────────────────────────────────

void notify(const char* json) {
  bleCharacteristic->setValue(json);
  if (deviceConnected) bleCharacteristic->notify();
  Serial.print("[BLE] Notificando: ");
  Serial.println(json);
}

void notifyOk(int machineIdx) {
  StaticJsonDocument<160> doc;
  doc["status"] = "OK";
  doc["machine"] = (machineIdx >= 0) ? MACHINE_NAMES[machineIdx] : "NONE";
  doc["remaining"] = (int)remainingSeconds();
  doc["paused"] = isPaused;
  char buf[160];
  serializeJson(doc, buf);
  notify(buf);
}

void notifyError(const char* msg) {
  StaticJsonDocument<128> doc;
  doc["status"] = "ERROR";
  doc["message"] = msg;
  char buf[128];
  serializeJson(doc, buf);
  notify(buf);
}

// ── Helpers ───────────────────────────────────────────────────

void blinkLed(int times, int delayMs = 150) {
  for (int i = 0; i < times * 2; i++) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    delay(delayMs);
  }
  digitalWrite(LED_PIN, LOW);
}

// ── Finalização da sessão ─────────────────────────────────────

void resetSession() {
  deactivateAll();
  sessionActive = false;
  activeMachine = -1;
  isPaused = false;
  totalSeconds = 0;
}

// ── Lógica dos comandos ───────────────────────────────────────

void cmdStart(int duration) {
  if (duration <= 0) {
    notifyError("duration invalido");
    return;
  }

  if (sessionActive) {
    // Heartbeat do totem — atualiza timer e retoma se estava pausado
    totalSeconds = duration;
    sessionStart = millis();
    elapsedPaused = 0;
    if (isPaused) {
      isPaused = false;
      pauseStart = 0;
      if (activeMachine >= 0) activateMachine(activeMachine);
      Serial.println("[SYNC] Sessão retomada via heartbeat START.");
    }
    Serial.print("[SYNC] Timer atualizado: ");
    Serial.print(duration / 60);
    Serial.print("min ");
    Serial.print(duration % 60);
    Serial.println("s restantes.");
    notifyOk(activeMachine);
    return;
  }

  deactivateAll();
  totalSeconds = duration;
  sessionStart = millis();
  elapsedPaused = 0;
  isPaused = false;
  activeMachine = -1;
  sessionActive = true;

  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║  SESSÃO INICIADA — AGUARDANDO SELECT  ║");
  Serial.print("║  Tempo total: ");
  Serial.print(duration / 60);
  Serial.print("min ");
  Serial.print(duration % 60);
  Serial.println("s");
  Serial.println("╚═══════════════════════════════════════╝");

  notifyOk(activeMachine);
}

void cmdSelect(const char* machineName) {
  if (!sessionActive) {
    notifyError("sessao nao ativa");
    return;
  }

  int idx = findMachine(machineName);
  if (idx < 0) {
    notifyError("maquina desconhecida");
    return;
  }

  activeMachine = idx;
  if (!isPaused) activateMachine(activeMachine);

  Serial.print("[CMD] SELECT → ");
  Serial.println(MACHINE_NAMES[activeMachine]);
  notifyOk(activeMachine);
}

void cmdPause() {
  if (!sessionActive) {
    notifyError("sessao nao ativa");
    return;
  }
  if (isPaused) {
    notifyOk(activeMachine);
    return;
  }

  isPaused = true;
  pauseStart = millis();
  deactivateAll();

  Serial.println("[CMD] PAUSE — máquina desligada, timer pausado.");
  notifyOk(activeMachine);
}

void cmdResume() {
  if (!sessionActive) {
    notifyError("sessao nao ativa");
    return;
  }
  if (!isPaused) {
    notifyOk(activeMachine);
    return;
  }

  elapsedPaused += millis() - pauseStart;
  isPaused = false;
  activateMachine(activeMachine);

  Serial.println("[CMD] RESUME — máquina reativada, timer retomado.");
  notifyOk(activeMachine);
}

void cmdStop() {
  Serial.println("[CMD] STOP — encerrando sessão.");
  resetSession();
  notify("{\"status\":\"DONE\"}");
}

void cmdStatus() {
  StaticJsonDocument<200> doc;
  doc["status"]    = "STATUS";
  doc["active"]    = sessionActive;
  doc["remaining"] = (int)remainingSeconds();
  doc["machine"]   = (sessionActive && activeMachine >= 0) ? MACHINE_NAMES[activeMachine] : "NONE";
  doc["paused"]    = isPaused;
  char buf[200];
  serializeJson(doc, buf);
  notify(buf);
  Serial.print("[STATUS] active=");
  Serial.print(sessionActive);
  Serial.print(" remaining=");
  Serial.print(remainingSeconds());
  Serial.print(" machine=");
  Serial.print((sessionActive && activeMachine >= 0) ? MACHINE_NAMES[activeMachine] : "NONE");
  Serial.print(" paused=");
  Serial.println(isPaused);
}

// ── Callback de escrita na characteristic ─────────────────────

class MyCharCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    String raw = ch->getValue().c_str();
    Serial.print("[BLE] Recebido: ");
    Serial.println(raw);
    lastBleActivity = millis();
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, raw)) {
      notifyError("JSON invalido");
      return;
    }

    const char* action = doc["action"] | "UNKNOWN";

    if (strcmp(action, "START") == 0) cmdStart(doc["duration"] | 0);
    else if (strcmp(action, "SELECT") == 0) cmdSelect(doc["machine"] | "");
    else if (strcmp(action, "PAUSE") == 0) cmdPause();
    else if (strcmp(action, "RESUME") == 0) cmdResume();
    else if (strcmp(action, "STOP") == 0) cmdStop();
    else if (strcmp(action, "STATUS") == 0) cmdStatus();
    else notifyError("acao desconhecida");
  }
};

// ── Callbacks de conexão ──────────────────────────────────────

class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    deviceConnected = true;
    Serial.println("\n╔════════════════════════════════╗");
    Serial.println(  "║  [BLE] Dispositivo CONECTADO   ║");
    Serial.println(  "╚════════════════════════════════╝\n");
  }
  void onDisconnect(BLEServer*) override {
    deviceConnected = false;
    wasConnected = true;
    if (sessionActive && !isPaused) {
      isPaused = true;
      pauseStart = millis();
      deactivateAll();
      Serial.println("[SEGURANÇA] BLE desconectado — sessão PAUSADA, máquinas desligadas (aguardando reconexão).");
    }
    Serial.println("\n╔════════════════════════════════╗");
    Serial.println(  "║ [BLE] Dispositivo DESCONECTADO ║");
    Serial.println(  "╚════════════════════════════════╝\n");
  }
};

void printBanner() {
  Serial.println("\n╔══════════════════════════════════════════╗");
  Serial.print  ("║  BliqEsp32 — ");
  Serial.print  (DEVICE_NAME);
  Serial.println("                    ║");
  Serial.println("╚══════════════════════════════════════════╝\n");
}

void logSessionStatus() {
  if (!sessionActive) return;

  unsigned long rem = remainingSeconds();
  int remMin = rem / 60;
  int remSec = rem % 60;
  const char* mName = (activeMachine >= 0) ? MACHINE_NAMES[activeMachine] : "NENHUMA";

  Serial.println("┌─────────────────────────────────────┐");
  Serial.print("│  Máquina ativa: ");
  Serial.println(mName);
  Serial.print("│  Restante: ");
  if (remMin < 10) Serial.print("0");
  Serial.print(remMin);
  Serial.print(":");
  if (remSec < 10) Serial.print("0");
  Serial.println(remSec);
  Serial.print("│  Status: ");
  Serial.println(isPaused ? "PAUSADO" : "EM ANDAMENTO");
  Serial.println("└─────────────────────────────────────┘");
}

// ── Setup ─────────────────────────────────────────────────────

void setup() {
  // Coloca todos os relés em LOW o quanto antes pra evitar pulso espúrio no boot
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  Serial.setTxTimeoutMs(1000);   // logo apos Serial.begin()
  delay(500);

  // Watchdog Timer — versão para ESP32 Arduino Core 3.x
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 10000,   // 10 segundos para reiniciar
    .idle_core_mask = 0,   // não monitora cores ociosos
    .trigger_panic = true  // reinicia o ESP32 ao travar
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);  // adiciona a task atual ao watchdog
  Serial.println("[WATCHDOG] Ativado — timeout de 10 segundos.");

  printBanner();

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new MyServerCallbacks());

  BLEService* service = bleServer->createService(SERVICE_UUID);

  bleCharacteristic = service->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  bleCharacteristic->addDescriptor(new BLE2902());
  bleCharacteristic->setCallbacks(new MyCharCallbacks());
  bleCharacteristic->setValue("{\"status\":\"READY\"}");

  service->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.println("[BLE] Advertising iniciado — aguardando app...");
  Serial.print("[BLE] Nome:               ");
  Serial.println(DEVICE_NAME);
  Serial.print("[BLE] Service UUID:        ");
  Serial.println(SERVICE_UUID);
  Serial.print("[BLE] Characteristic UUID: ");
  Serial.println(CHARACTERISTIC_UUID);
  Serial.print("[BLE] Comandos:            ");
  for (int i = 0; i < MACHINE_COUNT; i++) {
    Serial.print(MACHINE_NAMES[i]);
  }
  Serial.println();

  blinkLed(3);
}

// ── Loop ──────────────────────────────────────────────────────

void loop() {
  esp_task_wdt_reset();  // "ainda estou vivo"
  // Re-advertising imediato após desconexão
  if (wasConnected && !deviceConnected) {
    delay(500);
    BLEDevice::startAdvertising();
    wasConnected = false;
    lastAdvRestartMs = millis();
    Serial.println("[BLE] Re-advertising iniciado...");
  }

  // Re-advertising periódico enquanto desconectado
  if (!deviceConnected) {
    unsigned long now = millis();
    if (now - lastAdvRestartMs >= ADV_RESTART_INTERVAL) {
      lastAdvRestartMs = now;
      BLEDevice::startAdvertising();
      Serial.println("[BLE] Advertising reiniciado (keepalive)...");
    }
  }

  // LED de status
  if (!deviceConnected) {
    digitalWrite(LED_PIN, LOW);
  } else if (!sessionActive) {
    digitalWrite(LED_PIN, HIGH);
  } else if (isPaused) {
    digitalWrite(LED_PIN, (millis() / 1000) % 2);
  } else {
    digitalWrite(LED_PIN, (millis() / 250) % 2);
  }
  // Timeout BLE sem STOP
  if (sessionActive && deviceConnected) {
    if (millis() - lastBleActivity > BLE_TIMEOUT_MS) {
      Serial.println("[SEGURANÇA] Timeout BLE — encerrando sessão.");
      resetSession();
    }
  }
  // Timer de segurança: se o totem não enviar STOP a tempo, desliga as máquinas
  if (sessionActive && !isPaused && remainingSeconds() == 0) {
    Serial.println("\n╔══════════════════════════════════════════╗");
    Serial.println("║  TEMPO ESGOTADO — desligando máquinas    ║");
    Serial.println("╚══════════════════════════════════════════╝\n");
    resetSession();
    blinkLed(5, 100);
  }

  // Log periódico
  if (sessionActive) {
    unsigned long now = millis();
    if (now - lastLogTime >= LOG_INTERVAL_MS) {
      lastLogTime = now;
      logSessionStatus();
    }
  }

  delay(100);
}
