// ============================================================
//  BliqEsp32_TesteReles — Liga/desliga todos os relés simultaneamente
//  Placa: LIPXTECH "Angelo" — ESP32-S3 10-CHANNEL RELAY CONTROLLER
//
//  Aciona RL1..RL10 juntos por 3 segundos, desliga por 3 segundos,
//  e repete em loop. Log completo no Serial Monitor (115200 baud).
//
//  Objetivo: validar fiação, drivers ULN2003 e fonte de alimentação
//  sob carga (todos os relés puxando corrente ao mesmo tempo).
// ============================================================

#define RELAY_COUNT 10
#define LED_PIN 2
#define ON_DURATION_MS 3000
#define OFF_DURATION_MS 3000

// Mesma pinagem de BliqEsp32_Angelo.ino (BOARD_TIPO=1)
const uint8_t RELAY_PINS[RELAY_COUNT] = {
  4,   // RELAY_1  → MCU_C1
  5,   // RELAY_2  → MCU_C2
  6,   // RELAY_3  → MCU_C3
  7,   // RELAY_4  → MCU_C4
  8,   // RELAY_5  → MCU_C5
  9,   // RELAY_6  → MCU_C6
  10,  // RELAY_7  → MCU_C7
  11,  // RELAY_8  → MCU_C8
  12,  // RELAY_9  → MCU_C9
  13   // RELAY_10 → MCU_C10
};

void writeAll(uint8_t level) {
  for (int i = 0; i < RELAY_COUNT; i++) {
    digitalWrite(RELAY_PINS[i], level);
  }
}

void setup() {
  for (int i = 0; i < RELAY_COUNT; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], LOW);
  }
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("╔══════════════════════════════════════════╗");
  Serial.println("║  BliqEsp32 — TESTE ALL ON / ALL OFF      ║");
  Serial.println("╠══════════════════════════════════════════╣");
  Serial.print  ("║  ON:  ");
  Serial.print(ON_DURATION_MS);
  Serial.println(" ms                             ║");
  Serial.print  ("║  OFF: ");
  Serial.print(OFF_DURATION_MS);
  Serial.println(" ms                             ║");
  Serial.print  ("║  Relés no teste: ");
  Serial.print(RELAY_COUNT);
  Serial.println("                       ║");
  Serial.println("╚══════════════════════════════════════════╝");
  Serial.println();

  Serial.println("[MAP] Mapeamento pino ↔ relé:");
  for (int i = 0; i < RELAY_COUNT; i++) {
    Serial.print("      RL");
    Serial.print(i + 1);
    Serial.print("  → GPIO");
    Serial.print(RELAY_PINS[i]);
    Serial.print("  (MCU_C");
    Serial.print(i + 1);
    Serial.println(")");
  }
  Serial.println();
  Serial.println("[INFO] Iniciando ciclo em 2s...");
  delay(2000);
}

void loop() {
  static uint32_t cycle = 1;

  Serial.print("[CICLO #");
  Serial.print(cycle);
  Serial.print("] >>> ALL ON  (RL1..RL");
  Serial.print(RELAY_COUNT);
  Serial.println(")");
  writeAll(HIGH);
  digitalWrite(LED_PIN, HIGH);
  delay(ON_DURATION_MS);

  Serial.print("[CICLO #");
  Serial.print(cycle);
  Serial.println("] <<< ALL OFF");
  writeAll(LOW);
  digitalWrite(LED_PIN, LOW);
  delay(OFF_DURATION_MS);

  cycle++;
}
