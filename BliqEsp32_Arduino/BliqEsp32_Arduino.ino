// ============================================================
//  BliqEsp32 — Terminal BLE multi-máquina para Posto de Lavagem
//  Arduino IDE version
//
//  Dependencias (instalar pelo Library Manager):
//    - ArduinoJson  (by Benoit Blanchon)  versao 6.x
//
//  Placa: ESP32 Dev Module
//  Board Manager URL:
//    https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
//
//  Pinos das máquinas (relés):
//    PRE_LAVAGEM → GPIO 26
//    ESPUMA      → GPIO 27
//    ENXAGUE     → GPIO 14
//    FINALIZAR   → GPIO 12
//    LED status  → GPIO 2
//
//  Protocolo BLE — comandos recebidos (JSON):
//    { "action": "START",  "duration": <minutos> }
//    { "action": "SELECT", "machine": "<NOME>" }
//    { "action": "PAUSE"  }
//    { "action": "RESUME" }
//    { "action": "STOP"   }
//
//  Notificações enviadas (JSON):
//    { "status": "OK",   "machine": "<NOME>", "remaining": <segundos> }
//    { "status": "DONE"  }
//    { "status": "ERROR", "message": "<msg>" }
// ============================================================

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

// ── UUIDs (devem ser idênticos ao app BliqTeste) ─────────────
#define SERVICE_UUID        "12345678-1234-1234-1234-1234567890ab"
#define CHARACTERISTIC_UUID "abcd1234-5678-90ab-cdef-1234567890ab"
#define DEVICE_NAME         "ESP32_LAVAGEM"

// ── Pinos ────────────────────────────────────────────────────
#define LED_PIN 2
#define MACHINE_COUNT 4

const char* MACHINE_NAMES[MACHINE_COUNT] = {
  "PRE_LAVAGEM", "ESPUMA", "ENXAGUE", "FINALIZAR"
};
const int MACHINE_PINS[MACHINE_COUNT] = { 26, 27, 14, 12 };

// ── Estado global ─────────────────────────────────────────────
BLEServer*         bleServer         = nullptr;
BLECharacteristic* bleCharacteristic = nullptr;

bool deviceConnected = false;
bool wasConnected    = false;

// Sessão
bool         sessionActive  = false;
int          activeMachine  = -1;      // índice em MACHINE_NAMES/-1 = nenhuma
bool         isPaused       = false;
int          totalMinutes   = 0;
unsigned long sessionStart  = 0;       // millis do início da sessão
unsigned long pauseStart    = 0;       // millis de quando pausou
unsigned long elapsedPaused = 0;       // total de ms em pausa

unsigned long lastLogTime = 0;
#define LOG_INTERVAL_MS 10000

// ── Helpers de máquina ────────────────────────────────────────

void deactivateAll() {
  for (int i = 0; i < MACHINE_COUNT; i++) {
    digitalWrite(MACHINE_PINS[i], LOW);
  }
}

void activateMachine(int idx) {
  deactivateAll();
  if (idx >= 0 && idx < MACHINE_COUNT) {
    digitalWrite(MACHINE_PINS[idx], HIGH);
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
  unsigned long totalMs = (unsigned long)totalMinutes * 60000UL;
  unsigned long elapsed = millis() - sessionStart - elapsedPaused;
  if (isPaused) {
    unsigned long pauseMs = millis() - pauseStart;
    elapsed = (millis() - sessionStart) - elapsedPaused - pauseMs;
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
  StaticJsonDocument<128> doc;
  doc["status"]    = "OK";
  doc["machine"]   = (machineIdx >= 0) ? MACHINE_NAMES[machineIdx] : "NONE";
  doc["remaining"] = (int)remainingSeconds();
  doc["paused"]    = isPaused;
  char buf[128];
  serializeJson(doc, buf);
  notify(buf);
}

void notifyError(const char* msg) {
  StaticJsonDocument<128> doc;
  doc["status"]  = "ERROR";
  doc["message"] = msg;
  char buf[128];
  serializeJson(doc, buf);
  notify(buf);
}

// ── Lógica dos comandos ───────────────────────────────────────

void cmdStart(int duration) {
  if (duration <= 0) { notifyError("duration invalido"); return; }

  deactivateAll();
  totalMinutes  = duration;
  sessionStart  = millis();
  elapsedPaused = 0;
  isPaused      = false;
  activeMachine = 0; // começa com PRE_LAVAGEM
  sessionActive = true;

  activateMachine(activeMachine);

  Serial.println("╔═══════════════════════════════════════╗");
  Serial.println("║      SESSÃO INICIADA — PRE_LAVAGEM    ║");
  Serial.print  ("║  Tempo total: "); Serial.print(duration); Serial.println(" min");
  Serial.println("╚═══════════════════════════════════════╝");

  notifyOk(activeMachine);
}

void cmdSelect(const char* machineName) {
  if (!sessionActive) { notifyError("sessao nao ativa"); return; }

  int idx = findMachine(machineName);
  if (idx < 0) { notifyError("maquina desconhecida"); return; }

  if (isPaused) {
    // troca a máquina mas mantém pausado
    activeMachine = idx;
    deactivateAll();
  } else {
    activeMachine = idx;
    activateMachine(activeMachine);
  }

  Serial.print("[CMD] SELECT → "); Serial.println(MACHINE_NAMES[activeMachine]);
  notifyOk(activeMachine);
}

void cmdPause() {
  if (!sessionActive)   { notifyError("sessao nao ativa"); return; }
  if (isPaused)         { notifyOk(activeMachine); return; }

  isPaused   = true;
  pauseStart = millis();
  deactivateAll();

  Serial.println("[CMD] PAUSE — máquina desligada, timer pausado.");
  notifyOk(activeMachine);
}

void cmdResume() {
  if (!sessionActive)   { notifyError("sessao nao ativa"); return; }
  if (!isPaused)        { notifyOk(activeMachine); return; }

  elapsedPaused += millis() - pauseStart;
  isPaused = false;
  activateMachine(activeMachine);

  Serial.println("[CMD] RESUME — máquina reativada, timer retomado.");
  notifyOk(activeMachine);
}

void cmdStop() {
  deactivateAll();
  sessionActive = false;
  activeMachine = -1;
  isPaused      = false;

  Serial.println("[CMD] STOP — sessão encerrada.");
  notify("{\"status\":\"DONE\"}");
}

// ── Callback de escrita na characteristic ─────────────────────

class MyCharCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* ch) override {
    String raw = ch->getValue().c_str();
    Serial.print("[BLE] Recebido: "); Serial.println(raw);

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, raw)) { notifyError("JSON invalido"); return; }

    const char* action = doc["action"] | "UNKNOWN";

    if      (strcmp(action, "START")  == 0) cmdStart(doc["duration"] | 0);
    else if (strcmp(action, "SELECT") == 0) cmdSelect(doc["machine"] | "");
    else if (strcmp(action, "PAUSE")  == 0) cmdPause();
    else if (strcmp(action, "RESUME") == 0) cmdResume();
    else if (strcmp(action, "STOP")   == 0) cmdStop();
    else                                    notifyError("acao desconhecida");
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
    wasConnected    = true;
    // sessão continua internamente como segurança (timer de fallback)
    Serial.println("\n╔════════════════════════════════╗");
    Serial.println(  "║ [BLE] Dispositivo DESCONECTADO ║");
    Serial.println(  "╚════════════════════════════════╝\n");
  }
};

// ── Helpers ───────────────────────────────────────────────────

void blinkLed(int times, int delayMs = 150) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(delayMs);
    digitalWrite(LED_PIN, LOW);  delay(delayMs);
  }
}

void printBanner() {
  Serial.println("\n╔══════════════════════════════════════════╗");
  Serial.println(  "║  BliqEsp32 — Posto de Lavagem            ║");
  Serial.println(  "║  Multi-máquina BLE                       ║");
  Serial.println(  "╚══════════════════════════════════════════╝\n");
}

void logSessionStatus() {
  if (!sessionActive) return;

  unsigned long rem = remainingSeconds();
  int remMin = rem / 60;
  int remSec = rem % 60;
  const char* mName = (activeMachine >= 0) ? MACHINE_NAMES[activeMachine] : "NENHUMA";

  Serial.println("┌─────────────────────────────────────┐");
  Serial.print  ("│  Máquina ativa: "); Serial.println(mName);
  Serial.print  ("│  Restante: ");
  if (remMin < 10) Serial.print("0");
  Serial.print(remMin); Serial.print(":");
  if (remSec < 10) Serial.print("0");
  Serial.println(remSec);
  Serial.print  ("│  Status: "); Serial.println(isPaused ? "PAUSADO" : "EM ANDAMENTO");
  Serial.println("└─────────────────────────────────────┘");
}

// ── Setup ─────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  for (int i = 0; i < MACHINE_COUNT; i++) {
    pinMode(MACHINE_PINS[i], OUTPUT);
    digitalWrite(MACHINE_PINS[i], LOW);
  }

  printBanner();

  BLEDevice::init(DEVICE_NAME);
  BLEDevice::setPower(ESP_PWR_LVL_P9);

  bleServer = BLEDevice::createServer();
  bleServer->setCallbacks(new MyServerCallbacks());

  BLEService* service = bleServer->createService(SERVICE_UUID);

  bleCharacteristic = service->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_READ  |
    BLECharacteristic::PROPERTY_WRITE |
    BLECharacteristic::PROPERTY_NOTIFY
  );
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
  Serial.print  ("[BLE] Nome:               "); Serial.println(DEVICE_NAME);
  Serial.print  ("[BLE] Service UUID:        "); Serial.println(SERVICE_UUID);
  Serial.print  ("[BLE] Characteristic UUID: "); Serial.println(CHARACTERISTIC_UUID);
  Serial.print  ("[BLE] Máquinas:            ");
  for (int i = 0; i < MACHINE_COUNT; i++) {
    Serial.print(MACHINE_NAMES[i]); Serial.print("(pin"); Serial.print(MACHINE_PINS[i]); Serial.print(") ");
  }
  Serial.println();

  blinkLed(3);
}

// ── Loop ──────────────────────────────────────────────────────

void loop() {
  // Re-advertising após desconexão
  if (wasConnected && !deviceConnected) {
    delay(500);
    BLEDevice::startAdvertising();
    wasConnected = false;
    Serial.println("[BLE] Re-advertising iniciado...");
  }

  // LED de status
  if (!deviceConnected) {
    // desconectado: apagado
    digitalWrite(LED_PIN, LOW);
  } else if (!sessionActive) {
    // conectado sem sessão: fixo
    digitalWrite(LED_PIN, HIGH);
  } else if (isPaused) {
    // pausado: pisca lento (1s)
    digitalWrite(LED_PIN, (millis() / 1000) % 2);
  } else {
    // em andamento: pisca rápido (250ms)
    digitalWrite(LED_PIN, (millis() / 250) % 2);
  }

  // Fallback: tempo esgotado no ESP32 (segurança caso BLE caia)
  if (sessionActive && !isPaused && remainingSeconds() == 0) {
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println(  "║  TEMPO ESGOTADO — ENCERRANDO SESSÃO  ║");
    Serial.println(  "╚══════════════════════════════════════╝\n");
    deactivateAll();
    sessionActive = false;
    activeMachine = -1;
    if (deviceConnected) notify("{\"status\":\"DONE\"}");
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
