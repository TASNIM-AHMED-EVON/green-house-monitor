/*
 * Smart Room Monitor — ESP32 Firmware
 *
 * Required libraries (install via Arduino Library Manager):
 *   - DHT sensor library        by Adafruit
 *   - Adafruit Unified Sensor   by Adafruit
 *   - Adafruit SSD1306          by Adafruit
 *   - Adafruit GFX Library      by Adafruit
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
 *   OLED SDA      → GPIO 21   (I2C — module has built-in pull-ups)
 *   OLED SCL      → GPIO 22
 *   OLED VCC      → 3.3V
 *   Relay IN1     → GPIO 26   (Fan — HIGH = ON)
 *   Relay IN2     → GPIO 27   (Light — HIGH = ON)
 *   Relay IN3     → GPIO 33   (O2 Pump — HIGH = ON)
 *   Relay VCC     → 5V        (coil needs 5V)
 *   Buzzer (+)    → GPIO 25   (active piezo — HIGH = beep)
 *   Buzzer (-)    → GND
 *
 * ── O2 Pump sub-circuit (relay channel 3, low-voltage only) ────────
 *   This is a 5V DC pump, NOT a mains device. It is switched on the
 *   5V rail through relay channel 3 — do NOT wire it to AC mains.
 *     5V adapter (+) → Relay3 COM
 *     Relay3 NO      → Pump (+)
 *     Pump (-)       → 5V adapter GND (common GND with ESP32)
 *   No O2 sensor is used. The MQ-135 reading is used as a CO2/stale-air
 *   proxy: when air quality crosses CO2_THRESHOLD, we assume O2 is
 *   correspondingly depleted and turn the pump on to aerate/replenish.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── WiFi & Server ──────────────────────────────────────────────────
const char* WIFI_SSID  = "LSH_New_101";
const char* WIFI_PASS  = "99747305";

// Local testing (same network):  "http://192.168.x.x:3000/api/data"
// Render deployment:             "https://your-app-name.onrender.com/api/data"
const char* SERVER_URL = "https://green-house-monitor.onrender.com//api/data";

// ── Pin definitions ────────────────────────────────────────────────
#define DHT_PIN      4
#define DHT_TYPE     DHT22
#define MQ135_PIN    36
#define PIR_PIN      32
#define OLED_SDA     21
#define OLED_SCL     22
#define FAN_RELAY    26
#define LIGHT_RELAY  27
#define PUMP_RELAY   33
#define BUZZER_PIN   25

// CO2 proxy threshold (MQ-135 12-bit ADC) — no O2 sensor available,
// so we trigger the O2 pump when air quality crosses this value.
// Tune alongside AQ_ALERT_THRESHOLD after burn-in/calibration.
const int CO2_PUMP_THRESHOLD = 700;

// ── OLED ───────────────────────────────────────────────────────────
#define SCREEN_W  128
#define SCREEN_H   64
#define OLED_ADDR 0x3C
Adafruit_SSD1306 oled(SCREEN_W, SCREEN_H, &Wire, -1);

// ── DHT ────────────────────────────────────────────────────────────
DHT dht(DHT_PIN, DHT_TYPE);

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

// ── setup ──────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== Smart Room Monitor ===");

  // Output pins
  pinMode(FAN_RELAY,   OUTPUT);
  pinMode(LIGHT_RELAY, OUTPUT);
  pinMode(PUMP_RELAY,  OUTPUT);
  pinMode(BUZZER_PIN,  OUTPUT);
  digitalWrite(FAN_RELAY,   LOW);
  digitalWrite(LIGHT_RELAY, LOW);
  digitalWrite(PUMP_RELAY,  LOW);
  digitalWrite(BUZZER_PIN,  LOW);

  // Input pin
  pinMode(PIR_PIN, INPUT);

  // DHT22
  dht.begin();

  // OLED
  Wire.begin(OLED_SDA, OLED_SCL);
  bool oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOk) {
    oled.clearDisplay();
    oled.setTextColor(SSD1306_WHITE);
    oled.setTextSize(1);
    oled.setCursor(0, 0);
    oled.println("Smart Room Monitor");
    oled.println("Starting...");
    oled.display();
  } else {
    Serial.println("[WARN] OLED not found — check wiring and address");
  }

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
    if (oledOk) {
      oled.clearDisplay();
      oled.setCursor(0, 0);
      oled.println("WiFi connected");
      oled.println(WiFi.localIP().toString());
      oled.display();
    }
    delay(1500);
  } else {
    Serial.println("\n[WARN] WiFi failed — running offline");
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

  // 4. Compute output states
  if (ctrlMode == "auto") {
    // Auto: sensors decide
    fanOn    = occupied && (lastTemp > 30.0f);
    lightOn  = occupied;
    buzzerOn = (lastAQ > 600);
    pumpOn   = (lastAQ > CO2_PUMP_THRESHOLD);  // no O2 sensor: CO2 proxy trigger
  }
  // Manual: fanOn / lightOn / buzzerOn / pumpOn already set from server response

  // 5. Apply to hardware
  digitalWrite(FAN_RELAY,   fanOn    ? HIGH : LOW);
  digitalWrite(LIGHT_RELAY, lightOn  ? HIGH : LOW);
  digitalWrite(PUMP_RELAY,  pumpOn   ? HIGH : LOW);
  digitalWrite(BUZZER_PIN,  buzzerOn ? HIGH : LOW);

  // 6. Update OLED
  updateOLED();

  // 7. POST to server every 5 s, read commands in response
  if (now - lastPost >= POST_INTERVAL) {
    sendToServer();
    lastPost = now;
  }

  delay(50);
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
  StaticJsonDocument<320> body;
  body["temp"]     = lastTemp;
  body["humidity"] = lastHum;
  body["aq"]       = lastAQ;
  body["occupied"] = occupied ? 1 : 0;
  body["pump"]     = pumpOn ? 1 : 0;

  String payload;
  serializeJson(body, payload);

  int code = http.POST(payload);
  Serial.printf("[POST] %d | T:%.1f H:%.1f AQ:%d Occ:%d Mode:%s\n",
    code, lastTemp, lastHum, lastAQ, (int)occupied, ctrlMode.c_str());

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

// ── updateOLED ─────────────────────────────────────────────────────
void updateOLED() {
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);

  // Line 0: Temp and humidity
  oled.setCursor(0, 0);
  oled.print("T:"); oled.print(lastTemp, 1);
  oled.print("C  H:"); oled.print(lastHum, 0); oled.print("%");

  // Line 1: Air quality and occupancy
  oled.setCursor(0, 10);
  oled.print("AQ:"); oled.print(lastAQ);
  oled.print("  "); oled.print(occupied ? "Occupied" : "Empty");

  // Line 2: Mode
  oled.setCursor(0, 22);
  oled.print("Mode: "); oled.print(ctrlMode == "auto" ? "AUTO" : "MANUAL");

  // Line 3: Fan and Light
  oled.setCursor(0, 34);
  oled.print("Fan:"); oled.print(fanOn ? "ON  " : "OFF ");
  oled.print("Lgt:"); oled.print(lightOn ? "ON" : "OFF");

  // Line 4: Buzzer and Pump
  oled.setCursor(0, 44);
  oled.print("Buzz:"); oled.print(buzzerOn ? "ON " : "OFF ");
  oled.print("O2:"); oled.print(pumpOn ? "ON" : "OFF");

  // Line 5: IP
  oled.setCursor(0, 54);
  if (WiFi.status() == WL_CONNECTED) {
    oled.print(WiFi.localIP().toString());
  } else {
    oled.print("No WiFi");
  }

  oled.display();
}
