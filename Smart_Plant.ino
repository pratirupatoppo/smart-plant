/*
  SmartPlant ESP32 — FINAL WORKING CODE
  ══════════════════════════════════════════════════════════════
  
  What it does:
    1. Reads soil moisture from capacitive sensor
    2. Calculates moisture percentage (0%=bone dry, 100%=soaking wet)
    3. Turns ON motor/pump when soil is DRY (< 30%)
    4. Turns OFF motor/pump when soil is WET ENOUGH (> 60%)
    5. Shows live data on OLED with cute face animations
    6. Sends data to backend API every 5 seconds
    7. Reads temperature & humidity from DHT11

  WIRING:
  ────────────────────────────────────────────────────────────
  OLED:   VCC→3.3V  GND→GND  SDA→GPIO21  SCL→GPIO22
  Soil:   VCC→VIN(5V)  GND→GND  AO→D32 (pin labeled D32)
  DHT11:  VCC→3.3V  GND→GND  DATA→GPIO4
  Relay:  VCC→5V   GND→GND  IN→GPIO26
  Pump:   Battery(+)→COM  NO→Pump(+)  Pump(-)→Battery(-)

  CALIBRATION:
  ────────────────────────────────────────────────────────────
  Serial Monitor @ 115200:
    R = reset calibration to defaults
    D = set DRY point (hold sensor in air)
    W = set WET point (dip sensor tip in water)
    C = check current values
    L = live monitor (see raw ADC value updating)
    H = help
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>

// ─── DISPLAY ────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ─── DHT11 ──────────────────────────────────────────────────────
#define DHT_PIN  4
DHT dht(DHT_PIN, DHT11);

// ─── NVS ────────────────────────────────────────────────────────
Preferences prefs;

// ─── WIFI / API CONFIG ──────────────────────────────────────────
const char* WIFI_SSID  = "OPPO";
const char* WIFI_PASS  = "yoman123";
const char* API_URL    = "https://smartplant-4j1b.onrender.com/api/esp/sensor";
const char* DEVICE_KEY = "abc123xyz";
const char* ZONE_ID    = "A";

// ─── PINS ───────────────────────────────────────────────────────
const int RELAY_PIN = 26;     // Relay IN
const int SOIL_PIN  = 33;     // Wire to pin LABELED "D32" on board
                               // (D32/D33 are swapped on this CH340C board)

// ─── MOTOR THRESHOLDS ───────────────────────────────────────────
// Motor turns ON when moisture drops BELOW this
const float MOTOR_ON_THRESHOLD  = 30.0;   // 30% = dry soil
// Motor turns OFF when moisture rises ABOVE this
const float MOTOR_OFF_THRESHOLD = 60.0;   // 60% = wet enough
// The gap between ON and OFF prevents rapid on/off switching

// ─── CALIBRATION DEFAULTS (raw ADC 0–4095) ──────────────────────
// Sensor reads HIGH when DRY, LOW when WET
uint32_t dryRaw = 3200;   // typical dry-air reading
uint32_t wetRaw = 1500;   // typical in-water reading

// ─── TIMING ─────────────────────────────────────────────────────
const unsigned long SEND_INTERVAL = 5000;   // send to API every 5 sec
const unsigned long WIFI_TIMEOUT  = 15000;  // WiFi connect timeout

// ─── SMOOTHING ──────────────────────────────────────────────────
#define RING_SIZE 8
uint32_t ringBuf[RING_SIZE];
int ringIdx = 0;
bool ringFull = false;

// ─── RUNTIME VARIABLES ─────────────────────────────────────────
float    soilPct      = 0.0;
uint32_t lastSoilRaw  = 0;
bool     motorOn      = false;
float    tempC        = 25.0;
float    humidity     = 50.0;
String   prediction   = "Low";
bool     wifiOK       = false;
int      lastHttp     = 0;
unsigned long lastSend = 0;

// ═══════════════════════════════════════════════════════════════
// RELAY CONTROL
// ═══════════════════════════════════════════════════════════════

void motorON() {
  motorOn = true;
  digitalWrite(RELAY_PIN, LOW);   // active-LOW relay
  Serial.println("[MOTOR] ON — watering");
}

void motorOFF() {
  motorOn = false;
  digitalWrite(RELAY_PIN, HIGH);  // HIGH = relay OFF
  Serial.println("[MOTOR] OFF");
}

// ═══════════════════════════════════════════════════════════════
// WIFI
// ═══════════════════════════════════════════════════════════════

void wifiOff() {
  WiFi.disconnect(false);
  WiFi.mode(WIFI_OFF);
  delay(20);
  // Re-assert relay pin after WiFi mode change
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);
}

void wifiOn() {
  WiFi.mode(WIFI_STA);
  delay(10);
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) { wifiOK = true; return true; }
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > WIFI_TIMEOUT) { wifiOK = false; return false; }
    delay(250);
    yield();
  }
  wifiOK = true;
  Serial.print("[WIFI] Connected: ");
  Serial.println(WiFi.localIP());
  return true;
}

// ═══════════════════════════════════════════════════════════════
// SOIL MOISTURE READING
// ═══════════════════════════════════════════════════════════════

// Read with oversampling and outlier rejection
uint32_t readSoilRaw() {
  const int SAMPLES = 16;
  uint32_t buf[SAMPLES];

  for (int i = 0; i < SAMPLES; i++) {
    buf[i] = (uint32_t)analogRead(SOIL_PIN);
    delayMicroseconds(800);
  }

  // Sort (insertion sort)
  for (int i = 1; i < SAMPLES; i++) {
    uint32_t key = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
    buf[j+1] = key;
  }

  // Average middle 50% (throw away outliers)
  int lo = SAMPLES / 4;
  int hi = SAMPLES * 3 / 4;
  uint32_t sum = 0;
  for (int i = lo; i < hi; i++) sum += buf[i];
  return sum / (hi - lo);
}

// Full moisture read: WiFi off → read → smooth → calculate %
float readMoisture() {
  // Turn WiFi off for clean ADC reading
  wifiOff();

  uint32_t raw = readSoilRaw();

  // Turn WiFi back on
  wifiOn();

  // Ring buffer smoothing
  ringBuf[ringIdx] = raw;
  ringIdx = (ringIdx + 1) % RING_SIZE;
  if (ringIdx == 0) ringFull = true;

  int count = ringFull ? RING_SIZE : ringIdx;
  if (count < 1) count = 1;
  uint32_t sum = 0;
  for (int i = 0; i < count; i++) sum += ringBuf[i];
  lastSoilRaw = sum / count;

  // Map: dryRaw→0%, wetRaw→100%
  // (sensor reads HIGH=dry, LOW=wet)
  float pct;
  if (lastSoilRaw >= dryRaw)      pct = 0.0;
  else if (lastSoilRaw <= wetRaw) pct = 100.0;
  else pct = (float)(dryRaw - lastSoilRaw) * 100.0 / (float)(dryRaw - wetRaw);

  return pct;
}

// ═══════════════════════════════════════════════════════════════
// MOTOR DECISION — with hysteresis
// ═══════════════════════════════════════════════════════════════

void decideMotor() {
  if (!motorOn && soilPct < MOTOR_ON_THRESHOLD) {
    // Soil is dry → start watering
    motorON();
  }
  else if (motorOn && soilPct > MOTOR_OFF_THRESHOLD) {
    // Soil is wet enough → stop watering
    motorOFF();
  }
  // If moisture is between 30-60%, keep current state (hysteresis)
}

// ═══════════════════════════════════════════════════════════════
// DHT11
// ═══════════════════════════════════════════════════════════════

void readDHT() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t)) {
    humidity = h;
    tempC = t;
  }
}

// ═══════════════════════════════════════════════════════════════
// API
// ═══════════════════════════════════════════════════════════════

void sendToAPI() {
  if (!connectWiFi()) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, API_URL)) return;

  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);

  JsonDocument doc;
  doc["zone_id"]        = ZONE_ID;
  doc["soil_moisture"]  = soilPct;
  doc["temperature_c"]  = tempC;
  doc["humidity"]       = humidity;
  doc["rainfall_mm"]    = 0.0;
  doc["sunlight_hours"] = 7.0;
  doc["wind_speed_kmh"] = 2.0;

  String payload;
  serializeJson(doc, payload);
  lastHttp = http.POST(payload);

  if (lastHttp > 0) {
    String resp = http.getString();
    JsonDocument out;
    if (!deserializeJson(out, resp)) {
      prediction = String(out["prediction"] | "Low");

      // ─── Backend motor control (bridges software → hardware) ───
      // The backend/frontend can now control the pump via API response.
      // Manual overrides from the PWA have a 2-minute timeout.
      bool apiMotor = out["motor_on"] | false;
      if (apiMotor && !motorOn) {
        motorON();
        Serial.println("[API] Backend requested motor ON");
      } else if (!apiMotor && motorOn) {
        motorOFF();
        Serial.println("[API] Backend requested motor OFF");
      }
    }
  }
  http.end();
}

// ═══════════════════════════════════════════════════════════════
// CALIBRATION (NVS)
// ═══════════════════════════════════════════════════════════════

void loadCal() {
  dryRaw = prefs.getUInt("dry", 3200);
  wetRaw = prefs.getUInt("wet", 1500);
  if (dryRaw <= wetRaw) { dryRaw = 3200; wetRaw = 1500; }
  Serial.print("[CAL] DRY="); Serial.print(dryRaw);
  Serial.print(" WET="); Serial.println(wetRaw);
}

void saveCal() {
  prefs.putUInt("dry", dryRaw);
  prefs.putUInt("wet", wetRaw);
}

// ═══════════════════════════════════════════════════════════════
// SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════

void runLiveMonitor() {
  Serial.println("\n=== LIVE MONITOR (press any key to exit) ===\n");
  while (!Serial.available()) {
    uint32_t raw = analogRead(SOIL_PIN);
    Serial.print("  raw="); Serial.print(raw);
    Serial.print("  ");
    int bar = raw * 40 / 4095;
    for (int i = 0; i < 40; i++) Serial.print(i < bar ? '#' : '.');
    Serial.println();

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(0, 0); display.print("RAW:");
    display.setCursor(0, 20); display.print(raw);
    display.setTextSize(1);
    display.setCursor(0, 45); display.print("~");
    display.print(raw * 3300 / 4095); display.print(" mV");
    int b = raw * 120 / 4095;
    display.drawRect(4, 56, 120, 8, SSD1306_WHITE);
    if (b > 0) display.fillRect(4, 56, b, 8, SSD1306_WHITE);
    display.display();

    delay(250);
    yield();
  }
  while (Serial.available()) Serial.read();
  Serial.println("=== EXIT ===\n");
}

void handleSerial() {
  if (!Serial.available()) return;
  char cmd = toupper(Serial.read());
  while (Serial.available()) Serial.read();

  if (cmd == 'L') {
    runLiveMonitor();

  } else if (cmd == 'D') {
    wifiOff();
    uint32_t raw = readSoilRaw();
    wifiOn();
    dryRaw = raw;
    Serial.print("[CAL] DRY set to "); Serial.println(dryRaw);
    if (dryRaw > wetRaw + 300) {
      saveCal();
      Serial.println("[CAL] Saved!");
    } else {
      Serial.println("[CAL] Now dip in water and type W");
    }

  } else if (cmd == 'W') {
    wifiOff();
    uint32_t raw = readSoilRaw();
    wifiOn();
    wetRaw = raw;
    Serial.print("[CAL] WET set to "); Serial.println(wetRaw);
    if (dryRaw > wetRaw + 300) {
      saveCal();
      Serial.print("[CAL] Done! DRY="); Serial.print(dryRaw);
      Serial.print(" WET="); Serial.print(wetRaw);
      Serial.print(" range="); Serial.println(dryRaw - wetRaw);
    } else {
      Serial.println("[CAL] Error: DRY must be > WET by 300+");
    }

  } else if (cmd == 'C') {
    wifiOff();
    uint32_t raw = readSoilRaw();
    wifiOn();
    Serial.print("[CAL] DRY="); Serial.print(dryRaw);
    Serial.print(" WET="); Serial.print(wetRaw);
    Serial.print(" NOW="); Serial.print(raw);
    Serial.print(" soil="); Serial.print(soilPct, 1);
    Serial.println("%");

  } else if (cmd == 'R') {
    dryRaw = 3200; wetRaw = 1500;
    saveCal();
    Serial.println("[CAL] Reset to defaults (3200/1500)");

  } else if (cmd == 'H' || cmd == '?') {
    Serial.println("[HELP] L=live D=dry W=wet C=check R=reset H=help");
  }
}

// ═══════════════════════════════════════════════════════════════
// BITMAPS (32x32 face icons)
// ═══════════════════════════════════════════════════════════════

const unsigned char bmp_happy[] PROGMEM = {
  0x00,0x0f,0xf0,0x00,0x00,0x3f,0xfc,0x00,0x00,0x70,0x0e,0x00,0x00,0xe0,0x07,0x00,
  0x01,0xc0,0x03,0x80,0x03,0x80,0x01,0xc0,0x03,0x00,0x00,0xc0,0x07,0x00,0x00,0xe0,
  0x06,0x00,0x00,0x60,0x0e,0x33,0xcc,0x70,0x0c,0x33,0xcc,0x30,0x1c,0x00,0x00,0x38,
  0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x40,0x02,0x18,
  0x18,0x60,0x06,0x18,0x1c,0x30,0x0c,0x38,0x0c,0x1f,0xf8,0x30,0x0e,0x00,0x00,0x70,
  0x07,0x00,0x00,0xe0,0x03,0x80,0x01,0xc0,0x03,0xc0,0x03,0xc0,0x01,0xe0,0x07,0x80,
  0x00,0xf0,0x0f,0x00,0x00,0x78,0x1e,0x00,0x00,0x3f,0xfc,0x00,0x00,0x0f,0xf0,0x00
};

const unsigned char bmp_sad[] PROGMEM = {
  0x00,0x0f,0xf0,0x00,0x00,0x3f,0xfc,0x00,0x00,0x70,0x0e,0x00,0x00,0xe0,0x07,0x00,
  0x01,0xc0,0x03,0x80,0x03,0x80,0x01,0xc0,0x03,0x00,0x00,0xc0,0x07,0x00,0x00,0xe0,
  0x06,0x00,0x00,0x60,0x0e,0x33,0xcc,0x70,0x0c,0x1e,0x78,0x30,0x1c,0x00,0x00,0x38,
  0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x0f,0xf0,0x18,
  0x18,0x10,0x08,0x18,0x1c,0x20,0x04,0x38,0x0c,0x40,0x02,0x30,0x0e,0x00,0x00,0x70,
  0x07,0x00,0x00,0xe0,0x03,0x80,0x01,0xc0,0x03,0xc0,0x03,0xc0,0x01,0xe0,0x07,0x80,
  0x00,0xf0,0x0f,0x00,0x00,0x78,0x1e,0x00,0x00,0x3f,0xfc,0x00,0x00,0x0f,0xf0,0x00
};

const unsigned char bmp_drink[] PROGMEM = {
  0x00,0x0f,0xf0,0x00,0x00,0x3f,0xfc,0x00,0x00,0x70,0x0e,0x00,0x00,0xe0,0x07,0x00,
  0x01,0xc0,0x03,0x80,0x03,0x80,0x01,0xc0,0x03,0x00,0x00,0xc0,0x07,0x3c,0x3c,0xe0,
  0x06,0x7e,0x7e,0x60,0x0e,0x7e,0x7e,0x70,0x0c,0x3c,0x3c,0x30,0x1c,0x00,0x00,0x38,
  0x18,0x03,0xc0,0x18,0x18,0x0c,0x30,0x18,0x18,0x10,0x08,0x18,0x18,0x10,0x08,0x18,
  0x18,0x10,0x08,0x18,0x1c,0x0c,0x30,0x38,0x0c,0x03,0xc0,0x30,0x0e,0x00,0x00,0x70,
  0x07,0x00,0x00,0xe0,0x03,0x80,0x01,0xc0,0x03,0xc0,0x03,0xc0,0x01,0xe0,0x07,0x80,
  0x00,0xf0,0x0f,0x00,0x00,0x78,0x1e,0x00,0x00,0x3f,0xfc,0x00,0x00,0x0f,0xf0,0x00
};

// ═══════════════════════════════════════════════════════════════
// OLED DISPLAY
// ═══════════════════════════════════════════════════════════════

void drawStatus() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Soil: ");
  display.print((int)soilPct);
  display.print("%");
  if (motorOn) display.print(" [PUMP]");

  display.setCursor(0, 10);
  display.print("T:");
  display.print((int)tempC);
  display.print("C H:");
  display.print((int)humidity);
  display.print("% ");
  display.print(wifiOK ? "OK" : "--");
}

void showHappy(bool bounce) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawStatus();
  display.drawBitmap(48, bounce ? 18 : 22, bmp_happy, 32, 32, SSD1306_WHITE);
  // Footer
  display.setTextSize(1);
  display.setCursor(40, 56);
  display.print("HAPPY :)");
  display.display();
}

void showThirsty(bool shake) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawStatus();
  display.drawBitmap(shake ? 46 : 50, 20, bmp_sad, 32, 32, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(32, 56);
  display.print("THIRSTY :(");
  display.display();
}

void showWatering(bool dropHigh) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawStatus();
  display.drawBitmap(48, 20, bmp_drink, 32, 32, SSD1306_WHITE);
  display.fillCircle(64, dropHigh ? 14 : 18, 2, SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(28, 56);
  display.print("WATERING...");
  display.display();
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  // RELAY OFF IMMEDIATELY
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  motorOn = false;

  Serial.begin(115200);
  delay(500);
  Serial.println("\n================================");
  Serial.println("  SmartPlant — FINAL");
  Serial.println("================================\n");

  // OLED init
  Wire.begin(21, 22);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C) ||
      display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 20);
    display.print("SmartPlant");
    display.setCursor(30, 36);
    display.print("Starting...");
    display.display();
  }

  // DHT11
  dht.begin();

  // NVS + calibration
  prefs.begin("smartplant", false);
  loadCal();

  // Pre-warm ring buffer
  for (int i = 0; i < RING_SIZE; i++) {
    ringBuf[i] = (uint32_t)analogRead(SOIL_PIN);
    delay(10);
  }
  ringFull = true;
  ringIdx = 0;

  // First readings
  soilPct = readMoisture();
  readDHT();

  // WiFi + first API call
  connectWiFi();
  sendToAPI();

  // Make initial motor decision
  decideMotor();

  Serial.print("[BOOT] Soil: "); Serial.print(soilPct, 1);
  Serial.print("% raw:"); Serial.print(lastSoilRaw);
  Serial.print(" T:"); Serial.print(tempC, 1);
  Serial.print(" H:"); Serial.print(humidity, 1);
  Serial.print(" Motor:"); Serial.println(motorOn ? "ON" : "OFF");
  Serial.println("\n[CMD] L=live D=dry W=wet C=check R=reset H=help\n");
}

// ═══════════════════════════════════════════════════════════════
// MAIN LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  // Handle serial calibration commands
  handleSerial();

  // Every 5 seconds: read sensors, decide motor, send to API
  if (millis() - lastSend >= SEND_INTERVAL) {
    lastSend = millis();

    // Read all sensors
    soilPct = readMoisture();
    readDHT();

    // Decide: should motor be on or off?
    decideMotor();

    // Send data to backend
    sendToAPI();

    // Log to serial
    Serial.print("[LOOP] Soil:");
    Serial.print(soilPct, 1);
    Serial.print("% raw:");
    Serial.print(lastSoilRaw);
    Serial.print(" T:");
    Serial.print(tempC, 1);
    Serial.print(" H:");
    Serial.print(humidity, 1);
    Serial.print(" Motor:");
    Serial.print(motorOn ? "ON" : "OFF");
    Serial.print(" API:");
    Serial.println(lastHttp);
  }

  // Animate OLED based on state
  if (motorOn) {
    // Watering animation
    showWatering(true);  delay(150);
    showWatering(false); delay(150);
  } else if (soilPct < MOTOR_ON_THRESHOLD) {
    // Thirsty but motor hasn't kicked in yet
    showThirsty(true);  delay(120);
    showThirsty(false); delay(120);
  } else {
    // Happy plant!
    showHappy(false); delay(220);
    showHappy(true);  delay(220);
  }

  yield();
}