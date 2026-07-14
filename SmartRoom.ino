/*
 * Smart Room Monitor — ESP32 Firmware
 * (Sunstar 2004A 20x4 I2C LCD + HW-201 IR auto-close door with servo)
 *
 * Required libraries (install via Arduino Library Manager):
 *   - DHT sensor library        by Adafruit
 *   - Adafruit Unified Sensor   by Adafruit
 *   - LiquidCrystal I2C         by Frank de Brabander
 *   - ESP32Servo                by Kevin Harrington / madhephaestus   <-- NEW
 *   - ArduinoJson               by Benoit Blanchon  (v6.x)
 *   - WiFi                      (built-in ESP32)
 *   - HTTPClient                (built-in ESP32)
 *
 * ── Pin wiring ────────────────────────────────────────────────────
 *   DHT22 Data    → GPIO 4    (10 kΩ pull-up to 3.3V required)
 *   DHT22 VCC     → 3.3V
 *   MQ-135 AO     → GPIO 36   (ADC1 — voltage divider: 10k+10k needed)
 *   MQ-135 VCC    → 5V        (heater ~200 mA — preheat 24 h)
 *   PIR OUT       → GPIO 32   (3.3V TTL output — safe for ESP32)
 *   PIR VCC       → 5V
 *   LCD SDA       → GPIO 21   (I2C — backpack has built-in pull-ups)
 *   LCD SCL       → GPIO 22
 *   LCD VCC       → 5V        (I2C backpack needs 5V, NOT 3.3V)
 *   Relay IN1     → GPIO 26   (Fan)          [2-channel module, channel 1]
 *   Relay IN2     → GPIO 27   (O2 Pump)      [2-channel module, channel 2 — moved from Light]
 *   Relay IN3     → GPIO 19   (Light)        [1-channel module — moved from Pump]
 *   Relay VCC     → 5V        (coil needs 5V)
 *
 *   NOTE — active-LOW relay module: most common 1/2/3-channel relay
 *   boards energize the relay when IN is pulled LOW, and de-energize
 *   it when IN is HIGH (opposite of what you'd guess). This firmware
 *   accounts for that via RELAY_ACTIVE_LOW below and the setRelay()
 *   helper — all "on/off" logic elsewhere in the code stays in plain
 *   true/false terms; setRelay() is the only place that translates
 *   that into the correct HIGH/LOW for your specific board. If your
 *   relays still behave backwards after this fix, your board is
 *   actually active-HIGH — just flip RELAY_ACTIVE_LOW to false.
 *   Buzzer (+)    → GPIO 25   (active piezo — HIGH = beep)
 *   Buzzer (-)    → GND
 *   HW-201 OUT    → GPIO 34   (input-only pin — digital HIGH/LOW only, NOT a distance value)
 *   HW-201 VCC    → 5V
 *   HW-201 GND    → GND
 *   Servo signal  → GPIO 13   (PWM-capable)
 *   Servo VCC     → SEPARATE 5V supply (NOT ESP32 5V pin — current spikes can brown out the board)
 *   Servo GND     → common GND with ESP32 and the separate 5V supply
 *
 * ── Door design notes ─────────────────────────────────────────────
 *   The HW-201 is a DIGITAL obstacle sensor: its onboard potentiometer
 *   sets a fixed detection range, but the output is only HIGH/LOW —
 *   it does NOT report an actual distance in cm.
 *     - Mount the HW-201 so the door panel sits in its detection range
 *       when CLOSED, and moves out of range when OPEN.
 *     - Dashboard "Open" button → servo swings to DOOR_OPEN_ANGLE.
 *     - While the door stays open continuously past DOOR_ALARM_MS, the
 *       buzzer sounds — regardless of auto/manual mode — until closed.
 *     - Once someone physically shuts the door (IR back in range), if
 *       WE opened it via the dashboard, the servo auto-reverses back to
 *       DOOR_CLOSED_ANGLE and the alarm stops.
 *     - "Close now" on the dashboard still works as a manual override
 *       at any time.
 *     - IR_ACTIVE_LOW below controls polarity — most HW-201 boards pull
 *       OUT LOW when an object is detected in range. Flip it if your
 *       specific module behaves the opposite way (test with Serial
 *       Monitor first).
 *   If you need a real proportional close angle based on actual
 *   distance, swap the HW-201 for an analog sensor (e.g. Sharp
 *   GP2Y0A21) — the digital module simply can't provide that data.
 *
 * ── Relay module assignment (updated) ──────────────────────────────
 *   2-channel module:  channel 1 = Fan (GPIO26), channel 2 = O2 Pump (GPIO27)
 *   1-channel module:  Light (GPIO19)
 *   All three are 5V DC loads switched on the low-voltage rail — none
 *   of them are wired to AC mains.
 *     5V adapter (+) → relay COM (each module)
 *     Relay NO       → device (+)
 *     Device (-)     → common GND (shared with ESP32 and the adapter)
 *   No O2 sensor is used for the pump. The MQ-135 reading is used as a
 *   CO2/stale-air proxy: when air quality crosses CO2_PUMP_THRESHOLD,
 *   we assume O2 is correspondingly depleted and turn the pump on to
 *   aerate/replenish.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>          // NEW

// ── WiFi & Server ──────────────────────────────────────────────────
const char* WIFI_SSID  = "wifi-ssd";
const char* WIFI_PASS  = "password";

// Local testing (same network):  "http://192.168.x.x:3000/api/data"
// Render deployment:             "https://your-app-name.onrender.com/api/data"
const char* SERVER_URL = "https://green-house-monitor.onrender.com/api/data";

// ── Pin definitions ────────────────────────────────────────────────
#define DHT_PIN      4
#define DHT_TYPE     DHT22
#define MQ135_PIN    36
#define PIR_PIN      32
#define LCD_SDA      21
#define LCD_SCL      22
#define FAN_RELAY    26   // 2-channel module, channel 1
#define LIGHT_RELAY  19   // 1-channel module — moved here from PUMP's old pin
#define PUMP_RELAY   27   // 2-channel module, channel 2 — moved here from LIGHT's old pin
#define BUZZER_PIN   25
#define IR_DOOR_PIN  34   // HW-201 OUT — digital only
#define SERVO_PIN    13

// CO2 proxy threshold (MQ-135 12-bit ADC) — no O2 sensor available,
// so we trigger the O2 pump when air quality crosses this value.
const int CO2_PUMP_THRESHOLD = 700;

// ── Relay polarity ────────────────────────────────────────────────
// Most cheap relay boards are ACTIVE-LOW: IN=LOW energizes the relay,
// IN=HIGH releases it. Set to false if yours behaves the opposite way.
const bool RELAY_ACTIVE_LOW = true;

// Call this instead of digitalWrite() directly for FAN/LIGHT/PUMP relays.
// 'on' is always plain true=on / false=off; this handles the board's
// actual polarity so the rest of the code never has to think about it.
void setRelay(int pin, bool on) {
  digitalWrite(pin, RELAY_ACTIVE_LOW ? !on : on);
}

// ── Door config ──────────────────────────────────────────────────────
const bool IR_ACTIVE_LOW = true;               // most HW-201 boards: LOW = object detected. Flip if yours is inverted.
const unsigned long DOOR_ALARM_MS = 30000;     // 30 s open (after being actuated) before buzzer alarm starts
const int DOOR_CLOSED_ANGLE = 0;               // servo angle: door closed / latch engaged
const int DOOR_OPEN_ANGLE   = 90;              // servo angle: door open / latch released
// New door flow:
//   1. Website "Open" button → servo moves to DOOR_OPEN_ANGLE, doorActuatedOpen = true
//   2. IR sensor sees the door go out of range → doorOpen = true, timer starts
//   3. If still open after DOOR_ALARM_MS → buzzer sounds continuously (overrides
//      whatever the buzzer was doing in auto/manual mode) until the door closes
//   4. Someone physically shuts the door → IR sees it back in range → if we were
//      the ones who opened it (doorActuatedOpen), servo auto-reverses to
//      DOOR_CLOSED_ANGLE, and the alarm stops
//   "Close now" button still works as a manual override at any time.

// ── LCD (Sunstar 2004A, 20 cols x 4 rows, I2C backpack) ─────────────
#define LCD_ADDR  0x3F   // change to 0x3F if scanner finds that instead
#define LCD_COLS  20
#define LCD_ROWS  4
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);

// ── DHT ────────────────────────────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);

// ── Servo ────────────────────────────────────────────────────────────
Servo doorServo;

// ── Timings ────────────────────────────────────────────────────────
const unsigned long POST_INTERVAL    = 5000;    // ms between server POSTs
const unsigned long DHT_INTERVAL     = 2000;    // DHT22 minimum sample period
const unsigned long MOTION_TIMEOUT   = 300000;  // 5 min → room considered empty

// ── Runtime state ──────────────────────────────────────────────────
bool     occupied     = false;
unsigned long lastMotion   = 0;
unsigned long lastPost     = 0;
unsigned long lastDHTRead  = 0;

float lastTemp = 0.0f;
float lastHum  = 0.0f;
int   lastAQ   = 0;

// Command state received from server
String ctrlMode = "auto";   // "auto" | "manual"
bool   fanOn    = false;
bool   lightOn  = false;
bool   buzzerOn = false;
bool   pumpOn   = false;

// Door state
bool doorOpen           = false;  // current IR reading, true = open (not detected)
unsigned long doorOpenSince = 0;  // millis() when door first registered as open
bool doorActuatedOpen   = false;  // true after WE opened it via servo — waiting to auto-reverse on physical close
bool doorAlarmActive    = false;  // true once door has been open >= DOOR_ALARM_MS continuously

// ── setup ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Smart Room Monitor ===");

  // Output pins
  pinMode(FAN_RELAY,   OUTPUT);
  pinMode(LIGHT_RELAY, OUTPUT);
  pinMode(PUMP_RELAY,  OUTPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  setRelay(FAN_RELAY,   false);
  setRelay(LIGHT_RELAY, false);
  setRelay(PUMP_RELAY,  false);
  digitalWrite(BUZZER_PIN,  LOW);  // buzzer is a direct piezo, not on the relay board — unaffected

  // Input pins
  pinMode(PIR_PIN, INPUT);
  pinMode(IR_DOOR_PIN, INPUT);

  // DHT22
  dht.begin();

  // Servo
  doorServo.attach(SERVO_PIN);
  doorServo.write(DOOR_CLOSED_ANGLE);  // assume door starts closed

  // LCD
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Room Monitor");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  // Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi connected");
    lcd.setCursor(0, 1);
    lcd.print(WiFi.localIP().toString());
    delay(1500);
  } else {
    Serial.println("\n[WARN] WiFi failed — running offline");
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("WiFi failed");
    lcd.setCursor(0, 1);
    lcd.print("Running offline");
    delay(1500);
  }
}

// ── loop ───────────────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // 1. Read PIR (every loop — fast poll)
  if (digitalRead(PIR_PIN)) {
    occupied  = true;
    lastMotion = now;
  } else if (occupied && (now - lastMotion > MOTION_TIMEOUT)) {
    occupied = false;
  }

  // 2. Read DHT22 (every 2 s)
  if (now - lastDHTRead >= DHT_INTERVAL) {
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      lastTemp = t;
      lastHum  = h;
    }
    lastDHTRead = now;
  }

  // 3. Read MQ-135 AO (every loop)
  lastAQ = analogRead(MQ135_PIN);  // 12-bit: 0–4095

  // 4. Read HW-201 door sensor + auto-close logic
  updateDoorState(now);

  // 5. Compute output states
  if (ctrlMode == "auto") {
    // Auto: sensors decide
    fanOn    = occupied && (lastTemp > 30.0f);
    lightOn  = occupied;
    buzzerOn = (lastAQ > 600);
    pumpOn   = (lastAQ > CO2_PUMP_THRESHOLD);  // no O2 sensor: CO2 proxy trigger
  }
  // Manual: fanOn / lightOn / buzzerOn / pumpOn already set from server response

  // Door-open-too-long alarm overrides the buzzer regardless of mode —
  // this is a safety feature, not a toggle-able device.
  if (doorAlarmActive) buzzerOn = true;

  // 6. Apply to hardware
  setRelay(FAN_RELAY,   fanOn);
  setRelay(LIGHT_RELAY, lightOn);
  setRelay(PUMP_RELAY,  pumpOn);
  digitalWrite(BUZZER_PIN,  buzzerOn ? HIGH : LOW);  // direct piezo, not on relay board

  // 7. Update LCD
  updateLCD();

  // 8. POST to server every 5 s, read commands in response
  if (now - lastPost >= POST_INTERVAL) {
    sendToServer();
    lastPost = now;
  }

  delay(50);
}

// ── updateDoorState ──────────────────────────────────────────────────
// HW-201 gives only HIGH/LOW — no real distance, just in-range / out-of-range.
// New behaviour: we don't auto-close on a timer anymore. Instead:
//  - track how long the door has been continuously open, and sound the
//    buzzer once it passes DOOR_ALARM_MS
//  - when the door is physically shut again (IR back in range), if WE were
//    the ones who opened it via the dashboard, auto-reverse the servo shut
void updateDoorState(unsigned long now) {
  int raw = digitalRead(IR_DOOR_PIN);
  bool detected = IR_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
  bool currentlyOpen = !detected;  // door panel out of range = open

  if (currentlyOpen) {
    if (!doorOpen) {
      // just transitioned to open
      doorOpen        = true;
      doorOpenSince    = now;
      doorAlarmActive  = false;
      Serial.println("[Door] Open");
    }
    // still open — check alarm threshold
    if (!doorAlarmActive && (now - doorOpenSince >= DOOR_ALARM_MS)) {
      Serial.println("[Door] Open too long — alarm ON");
      doorAlarmActive = true;
    }
  } else {
    // door detected as closed
    if (doorOpen) {
      Serial.println("[Door] Closed / back in range");
      if (doorActuatedOpen) {
        doorServo.write(DOOR_CLOSED_ANGLE);
        doorActuatedOpen = false;
        Serial.println("[Door] Servo auto-reversed to closed position");
      }
    }
    doorOpen        = false;
    doorAlarmActive = false;
  }
}

// ── sendToServer ───────────────────────────────────────────────────
void sendToServer() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] reconnecting...");
    WiFi.reconnect();
    return;
  }

  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(6000);

  // Build JSON body
  StaticJsonDocument<384> body;
  body["temp"]      = lastTemp;
  body["humidity"]  = lastHum;
  body["aq"]        = lastAQ;
  body["occupied"]  = occupied ? 1 : 0;
  body["pump"]      = pumpOn ? 1 : 0;
  body["doorOpen"]    = doorOpen ? 1 : 0;   // dashboard can show door status
  body["doorAlarm"]   = doorAlarmActive ? 1 : 0;
  body["doorOpenSec"] = doorOpen ? (int)((millis() - doorOpenSince) / 1000) : 0;

  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  Serial.printf("[POST] %d | T:%.1f H:%.1f AQ:%d Occ:%d Door:%d Mode:%s\n",
    code, lastTemp, lastHum, lastAQ, (int)occupied, (int)doorOpen, ctrlMode.c_str());

  if (code == HTTP_CODE_OK) {
    String respStr = http.getString();
    StaticJsonDocument<320> resp;
    DeserializationError err = deserializeJson(resp, respStr);

    if (!err) {
      // Update mode
      String newMode = resp["mode"] | "auto";
      ctrlMode = newMode;

      // In manual mode, apply commands from server (browser set these)
      if (ctrlMode == "manual") {
        fanOn    = resp["commands"]["fan"]    | false;
        lightOn  = resp["commands"]["light"]  | false;
        buzzerOn = resp["commands"]["buzzer"] | false;
        pumpOn   = resp["commands"]["pump"]   | false;

        // Manual servo control from dashboard
        if (resp["commands"].containsKey("doorClose") && resp["commands"]["doorClose"] == true) {
          doorServo.write(DOOR_CLOSED_ANGLE);
          doorActuatedOpen = false;
          doorAlarmActive  = false;
          Serial.println("[Door] Manual CLOSE command — servo actuated");
        }
        if (resp["commands"].containsKey("doorOpen") && resp["commands"]["doorOpen"] == true) {
          doorServo.write(DOOR_OPEN_ANGLE);
          doorActuatedOpen = true;
          Serial.println("[Door] Manual OPEN command — servo actuated, waiting for physical close");
        }

        Serial.printf("[Manual] Fan:%d Light:%d Buzzer:%d Pump:%d\n",
          (int)fanOn, (int)lightOn, (int)buzzerOn, (int)pumpOn);
      }
    } else {
      Serial.println("[JSON] parse error: " + String(err.c_str()));
    }
  } else {
    Serial.printf("[POST] failed, code: %d\n", code);
  }

  http.end();
}

// ── updateLCD ──────────────────────────────────────────────────────
// 20x4 character layout:
//   Row 0: Temp + Humidity
//   Row 1: Air Quality + Occupancy
//   Row 2: Mode + Fan/Light/Pump/Buzzer states (abbreviated)
//   Row 3: IP address / door status
void updateLCD() {
  char line[LCD_COLS + 1];

  // Row 0: T:xx.xC  H:xx%
  lcd.setCursor(0, 0);
  snprintf(line, sizeof(line), "T:%4.1fC  H:%3.0f%%   ", lastTemp, lastHum);
  lcd.print(line);

  // Row 1: AQ:xxxx  Occ/Empty
  lcd.setCursor(0, 1);
  snprintf(line, sizeof(line), "AQ:%-4d  %-8s", lastAQ, occupied ? "Occupied" : "Empty");
  lcd.print(line);

  // Row 2: Mode  F:on L:on P:on B:on
  lcd.setCursor(0, 2);
  snprintf(line, sizeof(line), "%-4s F%d L%d P%d B%d",
    ctrlMode == "auto" ? "AUTO" : "MANU",
    fanOn ? 1 : 0, lightOn ? 1 : 0, pumpOn ? 1 : 0, buzzerOn ? 1 : 0);
  lcd.print(line);

  // Row 3: Door status takes priority display when open, else show IP
  lcd.setCursor(0, 3);
  String row3;
  if (doorOpen) {
    unsigned long openSec = (millis() - doorOpenSince) / 1000;
    row3 = doorAlarmActive ? ("ALARM! Open " + String(openSec) + "s")
                           : ("Door OPEN " + String(openSec) + "s");
  } else if (WiFi.status() == WL_CONNECTED) {
    row3 = WiFi.localIP().toString();
  } else {
    row3 = "No WiFi";
  }
  while (row3.length() < LCD_COLS) row3 += ' ';
  if (row3.length() > LCD_COLS) row3 = row3.substring(0, LCD_COLS);
  lcd.print(row3);
}