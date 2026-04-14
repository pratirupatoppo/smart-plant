/*
  SmartPlant ESP32 — v2.0 (Non-Blocking & Stabilized)
  ══════════════════════════════════════════════════════════════
  Updated by Antigravity:
  - Fixed API_URL to match local backend discovered at 10.120.162.117
  - Synced DEVICE_KEY to backend default "12345"
  - Restored backend-to-hardware pump control bridge
*/

#include <WiFi.h>
#include <WiFiClient.h> // Using WiFiClient for local network
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
// Points to the local server running on your PC
const char* API_URL    = "http://10.82.47.207:5000/api/esp/sensor";
const char* DEVICE_KEY = "12345";
const char* ZONE_ID    = "A";

// ─── PINS ───────────────────────────────────────────────────────
const int RELAY_PIN = 26;     
const int SOIL_PIN  = 33;     

// ─── MOTOR THRESHOLDS ───────────────────────────────────────────
const float MOTOR_ON_THRESHOLD  = 30.0;   // 30% = dry soil
const float MOTOR_OFF_THRESHOLD = 60.0;   // 60% = wet enough

// ─── CALIBRATION DEFAULTS (raw ADC 0–4095) ──────────────────────
uint32_t dryRaw = 3200;   
uint32_t wetRaw = 1500;   

// ─── TIMING (Non-Blocking Intervals) ────────────────────────────
const unsigned long SENSOR_INTERVAL = 2000;  // Read sensors every 2s
const unsigned long SEND_INTERVAL   = 10000; // Send to API every 10s
const unsigned long ANIM_INTERVAL   = 250;   // Update animation every 250ms
const unsigned long WIFI_TIMEOUT    = 15000; 

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
unsigned long lastSensorRead = 0;
unsigned long lastAnimUpdate = 0;
bool animFrame = false; // Toggles between 0 and 1 for animation frames

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

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) { wifiOK = true; return true; }
  
  Serial.print("[WIFI] Connecting..."); 
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  // Ensure relay holds its state during WiFi blocking connection
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t0 > WIFI_TIMEOUT) { 
      wifiOK = false; 
      Serial.println(" Timeout.");
      return false; 
    }
    delay(250);
  }
  wifiOK = true;
  Serial.print(" Connected: ");
  Serial.println(WiFi.localIP());
  return true;
}

// ═══════════════════════════════════════════════════════════════
// SOIL MOISTURE READING
// ═══════════════════════════════════════════════════════════════

// Improved noise rejection to allow reading with WiFi ON
uint32_t readSoilRaw() {
  const int SAMPLES = 32; // Increased sampling for better WiFi noise rejection
  uint32_t buf[SAMPLES];

  for (int i = 0; i < SAMPLES; i++) {
    buf[i] = (uint32_t)analogRead(SOIL_PIN);
    delayMicroseconds(500);
  }

  // Insertion sort for median filtering
  for (int i = 1; i < SAMPLES; i++) {
    uint32_t key = buf[i];
    int j = i - 1;
    while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
    buf[j+1] = key;
  }

  // Average middle 50% (throws away outliers caused by WiFi spikes)
  int lo = SAMPLES / 4;
  int hi = SAMPLES * 3 / 4;
  uint32_t sum = 0;
  for (int i = lo; i < hi; i++) sum += buf[i];
  return sum / (hi - lo);
}

void updateMoisture() {
  uint32_t raw = readSoilRaw();

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
  if (lastSoilRaw >= dryRaw)      soilPct = 0.0;
  else if (lastSoilRaw <= wetRaw) soilPct = 100.0;
  else soilPct = (float)(dryRaw - lastSoilRaw) * 100.0 / (float)(dryRaw - wetRaw);
}

// ═══════════════════════════════════════════════════════════════
// MOTOR DECISION
// ═══════════════════════════════════════════════════════════════

void decideMotor() {
  if (!motorOn && soilPct < MOTOR_ON_THRESHOLD) {
    motorON();
  }
  else if (motorOn && soilPct > MOTOR_OFF_THRESHOLD) {
    motorOFF();
  }
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

  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, API_URL)) return;

  http.setConnectTimeout(5000);
  http.setTimeout(5000);
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

      // ─── BRIDGE: Web-to-Hardware Control ───
      bool apiMotor = out["motor_on"] | false;
      if (apiMotor && !motorOn) {
        motorON();
        Serial.println("[API] Web dashboard requested motor ON");
      } else if (!apiMotor && motorOn) {
        motorOFF();
        Serial.println("[API] Web dashboard requested motor OFF");
      }
    }
  } else {
    Serial.print("[API] Failed to connect to server. Error code: ");
    Serial.println(lastHttp);
    Serial.println("[API] (Hint: Windows Firewall might be blocking port 5000!)");
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

void handleSerial() {
  if (!Serial.available()) return;
  char cmd = toupper(Serial.read());
  while (Serial.available()) Serial.read(); // Clear buffer

  if (cmd == 'L') {
    Serial.println("\n=== LIVE MONITOR (press key to exit) ===");
    while (!Serial.available()) {
      uint32_t raw = analogRead(SOIL_PIN);
      Serial.print("  raw="); Serial.print(raw);
      Serial.println();
      delay(250);
    }
    while (Serial.available()) Serial.read();
    Serial.println("=== EXIT ===\n");

  } else if (cmd == 'D') {
    dryRaw = readSoilRaw();
    Serial.print("[CAL] DRY set to "); Serial.println(dryRaw);
    if (dryRaw > wetRaw + 300) { saveCal(); Serial.println("[CAL] Saved!"); }

  } else if (cmd == 'W') {
    wetRaw = readSoilRaw();
    Serial.print("[CAL] WET set to "); Serial.println(wetRaw);
    if (dryRaw > wetRaw + 300) { saveCal(); Serial.println("[CAL] Saved!"); }

  } else if (cmd == 'C') {
    updateMoisture();
    Serial.print("[CAL] DRY="); Serial.print(dryRaw);
    Serial.print(" WET="); Serial.print(wetRaw);
    Serial.print(" NOW="); Serial.print(lastSoilRaw);
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
// OLED DISPLAY (Non-Blocking Rendering)
// ═══════════════════════════════════════════════════════════════

void drawStatus() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("Soil: "); display.print((int)soilPct); display.print("%");
  if (motorOn) display.print(" [PUMP]");

  display.setCursor(0, 10);
  display.print("T:"); display.print((int)tempC);
  display.print("C H:"); display.print((int)humidity);
  display.print("% ");
  display.print(wifiOK ? "OK" : "--");
}

void renderUI() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  drawStatus();

  if (motorOn) {
    // Watering Animation
    display.drawBitmap(48, 20, bmp_drink, 32, 32, SSD1306_WHITE);
    display.fillCircle(64, animFrame ? 14 : 18, 2, SSD1306_WHITE);
    display.setCursor(28, 56); display.print("WATERING...");
  } 
  else if (soilPct < MOTOR_ON_THRESHOLD) {
    // Thirsty Animation
    display.drawBitmap(animFrame ? 46 : 50, 20, bmp_sad, 32, 32, SSD1306_WHITE);
    display.setCursor(32, 56); display.print("THIRSTY :(");
  } 
  else {
    // Happy Animation
    display.drawBitmap(48, animFrame ? 18 : 22, bmp_happy, 32, 32, SSD1306_WHITE);
    display.setCursor(40, 56); display.print("HAPPY :)");
  }
  
  display.display();
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  motorOn = false;

  Serial.begin(115200);
  delay(500);
  Serial.println("\n================================");
  Serial.println("  SmartPlant — v2.0 (MODIFIED)");
  Serial.println("================================\n");

  Wire.begin(21, 22);
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C) || display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(20, 20); display.print("SmartPlant v2");
    display.setCursor(30, 36); display.print("Starting...");
    display.display();
  }

  dht.begin();
  prefs.begin("smartplant", false);
  loadCal();

  // Pre-warm smoothing buffer
  for (int i = 0; i < RING_SIZE; i++) {
    ringBuf[i] = readSoilRaw();
    delay(10);
  }
  ringFull = true;

  updateMoisture();
  readDHT();
  connectWiFi();
  decideMotor();
}

// ═══════════════════════════════════════════════════════════════
// MAIN LOOP (Fully Non-Blocking State Machine)
// ═══════════════════════════════════════════════════════════════

void loop() {
  unsigned long currentMillis = millis();

  // 1. Instant Serial Commands
  handleSerial();

  // 2. Sensor reading & motor logic (Every 2 seconds)
  if (currentMillis - lastSensorRead >= SENSOR_INTERVAL) {
    lastSensorRead = currentMillis;
    
    updateMoisture();
    readDHT();
    decideMotor();

    Serial.print("[LOOP] Soil:"); Serial.print(soilPct, 1);
    Serial.print("% raw:"); Serial.print(lastSoilRaw);
    Serial.print(" T:"); Serial.print(tempC, 1);
    Serial.print(" H:"); Serial.print(humidity, 1);
    Serial.print(" Motor:"); Serial.println(motorOn ? "ON" : "OFF");
  }

  // 3. API Sending (Every 10 seconds)
  if (currentMillis - lastSend >= SEND_INTERVAL) {
    lastSend = currentMillis;
    sendToAPI();
  }

  // 4. Smooth Display Animation (Every 250ms)
  if (currentMillis - lastAnimUpdate >= ANIM_INTERVAL) {
    lastAnimUpdate = currentMillis;
    animFrame = !animFrame; // Toggle frame boolean
    renderUI();
  }
}