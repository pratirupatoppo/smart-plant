#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DHT.h>
#include <esp_system.h>

// ─── STATE MACHINE ──────────────────────────────────────────────
enum PlantState { STATE_OPTIMAL, STATE_DRY, STATE_WATERED, STATE_ERROR };
PlantState currentState = STATE_OPTIMAL;

// ─── DISPLAY ────────────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// ─── DHT11 ──────────────────────────────────────────────────────
#define DHT_PIN  4
#define DHT_TYPE DHT11
DHT dht(DHT_PIN, DHT_TYPE);

// ─── NVS ────────────────────────────────────────────────────────
Preferences prefs;

// ─── USER CONFIG ────────────────────────────────────────────────
const char* WIFI_SSID  = "OPPO";
const char* WIFI_PASS  = "yoman123";
const char* API_URL    = "https://smartplant-4j1b.onrender.com/api/esp/sensor";
const char* DEVICE_KEY = "abc123xyz";
const char* ZONE_ID    = "A";

// ─── PINS ───────────────────────────────────────────────────────
const int RELAY_PIN = 26;
const int SOIL_PIN = 32;   // Wire goes to pin LABELED "D32" on board
                           // but board has D32/D33 swapped — so GPIO33 reads it

// ─── CALIBRATION (raw ADC 0–4095) ───────────────────────────────
#define DEFAULT_DRY_RAW  3200
#define DEFAULT_WET_RAW  1500
#define SENSOR_MIN_VALID   150
#define MIN_CAL_RANGE      300

// ─── TIMING ─────────────────────────────────────────────────────
const unsigned long SEND_INTERVAL_MS     = 5000;
const unsigned long WIFI_CONNECT_TIMEOUT = 18000;
const unsigned long PERSIST_INTERVAL_MS  = 10000;
const uint16_t      API_TIMEOUT_MS       = 8000;

// ─── SMOOTHING ──────────────────────────────────────────────────
#define ADC_SAMPLES  16
#define RING_SIZE     8

static uint32_t ringBuf[RING_SIZE];
static int      ringIdx  = 0;
static bool     ringFull = false;

// ─── CALIBRATION ────────────────────────────────────────────────
static uint32_t dryRaw = DEFAULT_DRY_RAW;
static uint32_t wetRaw = DEFAULT_WET_RAW;

// ─── STATE ──────────────────────────────────────────────────────
static bool sensorOk      = true;
static int  sensorFailCnt = 0;

unsigned long lastSendMs    = 0;
unsigned long lastPersistMs = 0;
float    soilPct       = 0.0f;
uint32_t lastSoilRaw   = 0;
bool     motorOn       = false;
String   prediction    = "Low";
bool     wifiConnected = false;
bool     backendReach  = false;
int      lastHttpCode  = 0;
float    tempC         = 28.0f;
float    humidity      = 65.0f;

// ═══════════════════════════════════════════════════════════════
// CALIBRATION
// ═══════════════════════════════════════════════════════════════

bool isCalValid(uint32_t dry, uint32_t wet) {
  return (dry > wet) && ((dry - wet) >= MIN_CAL_RANGE);
}

void loadCalibration() {
  uint32_t d = prefs.getUInt("dry_raw", DEFAULT_DRY_RAW);
  uint32_t w = prefs.getUInt("wet_raw", DEFAULT_WET_RAW);
  if (isCalValid(d, w)) { dryRaw = d; wetRaw = w; }
  else { dryRaw = DEFAULT_DRY_RAW; wetRaw = DEFAULT_WET_RAW; }
  Serial.print("[CAL] DRY="); Serial.print(dryRaw);
  Serial.print(" WET="); Serial.println(wetRaw);
}

void saveCalibration() {
  prefs.putUInt("dry_raw", dryRaw);
  prefs.putUInt("wet_raw", wetRaw);
}

void resetCalibration() {
  dryRaw = DEFAULT_DRY_RAW;
  wetRaw = DEFAULT_WET_RAW;
  saveCalibration();
  Serial.print("[CAL] Reset DRY="); Serial.print(dryRaw);
  Serial.print(" WET="); Serial.println(wetRaw);
}

// ═══════════════════════════════════════════════════════════════
// RELAY
// ═══════════════════════════════════════════════════════════════

void setRelay(bool on) {
  motorOn = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
}

void safeRelayOff() { setRelay(false); }

// ═══════════════════════════════════════════════════════════════
// WIFI
// ═══════════════════════════════════════════════════════════════

void wifiOff() {
  if (WiFi.status() == WL_CONNECTED || wifiConnected) {
    WiFi.disconnect(false);
    WiFi.mode(WIFI_OFF);
    delay(20);
  }
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);
}

void wifiOn() {
  if (WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
    delay(10);
  }
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);
}

const char* wifiStatusText(wl_status_t st) {
  switch (st) {
    case WL_IDLE_STATUS: return "IDLE";
    case WL_NO_SSID_AVAIL: return "NO_SSID_AVAIL";
    case WL_SCAN_COMPLETED: return "SCAN_COMPLETED";
    case WL_CONNECTED: return "CONNECTED";
    case WL_CONNECT_FAILED: return "CONNECT_FAILED";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED: return "DISCONNECTED";
    default: return "UNKNOWN";
  }
}

bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) { wifiConnected = true; return true; }
  Serial.print("[WIFI] Connecting to SSID: "); Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);
  unsigned long t0 = millis();
  unsigned long lastLog = 0;
  while (WiFi.status() != WL_CONNECTED) {
    wl_status_t st = WiFi.status();
    if (millis() - lastLog >= 1000) {
      lastLog = millis();
      Serial.print("[WIFI] status="); Serial.print((int)st);
      Serial.print(" ("); Serial.print(wifiStatusText(st)); Serial.println(")");
    }
    if (millis() - t0 > WIFI_CONNECT_TIMEOUT) {
      wifiConnected = false;
      Serial.print("[WIFI] Timeout. Last status="); Serial.print((int)st);
      Serial.print(" ("); Serial.print(wifiStatusText(st)); Serial.println(")");
      Serial.println("[WIFI] Check phone hotspot: 2.4GHz + WPA2 + SSID visible");
      return false;
    }
    delay(250); yield();
  }
  wifiConnected = true;
  Serial.print("[WIFI] status="); Serial.print((int)WiFi.status());
  Serial.print(" ("); Serial.print(wifiStatusText(WiFi.status())); Serial.println(")");
  Serial.print("[WIFI] IP: "); Serial.println(WiFi.localIP());
  return true;
}

// ═══════════════════════════════════════════════════════════════
// SOIL ADC — SINGLE PIN ONLY, NO MULTI-PIN SCANNING
// ═══════════════════════════════════════════════════════════════

// Read ONLY the selected SOIL_PIN with oversampling
uint32_t readSoilFiltered() {
  uint32_t buf[ADC_SAMPLES];
  for (int i = 0; i < ADC_SAMPLES; i++) {
    buf[i] = (uint32_t)analogRead(SOIL_PIN);
    delayMicroseconds(800);
    if (i % 4 == 0) yield();
  }

  // Sort
  for (int i = 1; i < ADC_SAMPLES; i++) {
    uint32_t key = buf[i]; int j = i - 1;
    while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
    buf[j+1] = key;
  }

  // Average middle 50%
  int lo = ADC_SAMPLES / 4, hi = ADC_SAMPLES * 3 / 4;
  uint32_t sum = 0;
  for (int i = lo; i < hi; i++) sum += buf[i];
  return sum / (hi - lo);
}

float readMoisturePercent() {
  wifiOff();
  uint32_t raw = readSoilFiltered();
  wifiOn();

  if (raw < SENSOR_MIN_VALID) {
    sensorFailCnt++;
    if (sensorFailCnt >= 3) sensorOk = false;
    return soilPct;
  }

  sensorOk = true;
  sensorFailCnt = 0;

  ringBuf[ringIdx] = raw;
  ringIdx = (ringIdx + 1) % RING_SIZE;
  if (ringIdx == 0) ringFull = true;

  int count = ringFull ? RING_SIZE : ringIdx;
  if (count < 1) count = 1;
  uint32_t s = 0;
  for (int i = 0; i < count; i++) s += ringBuf[i];
  lastSoilRaw = s / count;

  if (dryRaw <= wetRaw) { dryRaw = DEFAULT_DRY_RAW; wetRaw = DEFAULT_WET_RAW; saveCalibration(); }

  float pct;
  if (lastSoilRaw >= dryRaw) pct = 0.0f;
  else if (lastSoilRaw <= wetRaw) pct = 100.0f;
  else pct = (float)(dryRaw - lastSoilRaw) * 100.0f / (float)(dryRaw - wetRaw);

  Serial.print("[SOIL] pin:G"); Serial.print(SOIL_PIN);
  Serial.print(" raw:"); Serial.print(lastSoilRaw);
  Serial.print(" "); Serial.print(pct, 1); Serial.println("%");
  return pct;
}

// ─── DHT ────────────────────────────────────────────────────────
void readDHT() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  if (!isnan(h) && !isnan(t) && h >= 0 && h <= 100 && t > -20 && t < 85) {
    humidity = h; tempC = t;
  }
}

// ─── NVS ────────────────────────────────────────────────────────
void loadPersistedState() {
  soilPct = prefs.getFloat("soil", 0.0f);
  prediction = prefs.getString("pred", "Low");
  motorOn = false; setRelay(false);
}

void persistState(bool force) {
  if (!force && (millis() - lastPersistMs < PERSIST_INTERVAL_MS)) return;
  prefs.putFloat("soil", soilPct);
  prefs.putString("pred", prediction);
  lastPersistMs = millis();
}

// ─── API ────────────────────────────────────────────────────────
bool postSensorAndReadDecision() {
  soilPct = readMoisturePercent();
  readDHT();

  if (!sensorOk) {
    safeRelayOff(); persistState(false); currentState = STATE_ERROR;
    return false;
  }

  if (!connectWiFi()) {
    backendReach = false; safeRelayOff(); persistState(false); return false;
  }

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, API_URL)) {
    backendReach = false; safeRelayOff(); persistState(false); return false;
  }

  http.setConnectTimeout(API_TIMEOUT_MS);
  http.setTimeout(API_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Key", DEVICE_KEY);

  JsonDocument req;
  req["zone_id"] = ZONE_ID;
  req["soil_moisture"] = soilPct;
  req["temperature_c"] = tempC;
  req["humidity"] = humidity;
  req["rainfall_mm"] = 0.0;
  req["sunlight_hours"] = 7.0;
  req["wind_speed_kmh"] = 2.0;

  String payload; serializeJson(req, payload);
  lastHttpCode = http.POST(payload);

  if (lastHttpCode <= 0) {
    backendReach = false; safeRelayOff(); http.end(); persistState(false); return false;
  }

  String resp = http.getString(); http.end();

  JsonDocument out;
  if (deserializeJson(out, resp)) {
    backendReach = false; safeRelayOff(); persistState(false); return false;
  }

  bool apiMotor = out["motor_on"] | false;
  prediction = String(out["prediction"] | "Low");
  backendReach = true;
  if (apiMotor && sensorOk) setRelay(true); else setRelay(false);
  persistState(false);
  return true;
}

PlantState decideState() {
  if (!sensorOk) return STATE_ERROR;
  if (motorOn) return STATE_WATERED;
  String p = prediction; p.toLowerCase();
  if (soilPct < 35.0f || p == "high" || p == "medium") return STATE_DRY;
  return STATE_OPTIMAL;
}

// No pin auto-detect — using GPIO32 only

// ═══════════════════════════════════════════════════════════════
// LIVE MONITOR (single pin only)
// ═══════════════════════════════════════════════════════════════

void runLiveMonitor() {
  Serial.print("\n=== LIVE MONITOR GPIO"); Serial.print(SOIL_PIN);
  Serial.println(" ===");
  Serial.println("Press any key to exit\n");

  while (!Serial.available()) {
    uint32_t raw = analogRead(SOIL_PIN);
    Serial.print("  G"); Serial.print(SOIL_PIN);
    Serial.print(": "); Serial.print(raw);
    Serial.print("  ");
    int bar = raw * 40 / 4095;
    for (int i = 0; i < 40; i++) Serial.print(i < bar ? '#' : '.');
    Serial.println();

    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("LIVE G"); display.print(SOIL_PIN);
    display.setCursor(0, 16);
    display.print("Raw: "); display.print(raw);
    display.setCursor(0, 32);
    display.print("~"); display.print(raw * 3300 / 4095); display.print(" mV");
    int barPx = raw * 120 / 4095;
    display.drawRect(4, 50, 120, 10, SSD1306_WHITE);
    if (barPx > 0) display.fillRect(4, 50, barPx, 10, SSD1306_WHITE);
    display.display();

    delay(250);
    yield();
  }
  while (Serial.available()) Serial.read();
  Serial.println("\n=== EXIT LIVE ===\n");
}

// ═══════════════════════════════════════════════════════════════
// SERIAL COMMANDS
// ═══════════════════════════════════════════════════════════════

void handleSerialCal() {
  if (!Serial.available()) return;
  char cmd = toupper(Serial.read());
  while (Serial.available()) Serial.read();

  if (cmd == 'L') {
    runLiveMonitor();

  } else if (cmd == 'D') {
    wifiOff();
    uint32_t raw = readSoilFiltered();
    wifiOn();
    if (raw < 50) {
      Serial.print("[CAL] FAIL raw="); Serial.println(raw);
      Serial.println("[CAL] Type S to swap pin, or L for live monitor");
      return;
    }
    uint32_t old = dryRaw;
    dryRaw = raw;
    Serial.print("[CAL] DRY: "); Serial.print(dryRaw);
    Serial.print(" (was "); Serial.print(old); Serial.println(")");
    if (isCalValid(dryRaw, wetRaw)) {
      saveCalibration();
      Serial.print("[CAL] OK! range="); Serial.println(dryRaw - wetRaw);
    } else {
      Serial.println("[CAL] Now put sensor in water, type W");
    }

  } else if (cmd == 'W') {
    wifiOff();
    uint32_t raw = readSoilFiltered();
    wifiOn();
    if (raw < 50) {
      Serial.print("[CAL] FAIL raw="); Serial.println(raw);
      return;
    }
    uint32_t old = wetRaw;
    wetRaw = raw;
    Serial.print("[CAL] WET: "); Serial.print(wetRaw);
    Serial.print(" (was "); Serial.print(old); Serial.println(")");
    if (isCalValid(dryRaw, wetRaw)) {
      saveCalibration();
      Serial.print("[CAL] Done! DRY="); Serial.print(dryRaw);
      Serial.print(" WET="); Serial.print(wetRaw);
      Serial.print(" range="); Serial.println(dryRaw - wetRaw);
    } else {
      Serial.println("[CAL] Invalid (dry must be > wet by 300+)");
      wetRaw = old;
    }

  } else if (cmd == 'C') {
    wifiOff();
    uint32_t raw = readSoilFiltered();
    wifiOn();
    Serial.print("[CAL] G"); Serial.print(SOIL_PIN);
    Serial.print(" DRY="); Serial.print(dryRaw);
    Serial.print(" WET="); Serial.print(wetRaw);
    Serial.print(" LIVE="); Serial.print(raw);
    Serial.print(" soil="); Serial.print(soilPct, 1); Serial.println("%");

  } else if (cmd == 'R') {
    resetCalibration();

  } else if (cmd == 'H' || cmd == '?') {
    Serial.println("[HELP] L=live D=dry W=wet C=check R=reset");

  } else if (cmd != '\n' && cmd != '\r') {
    Serial.println("[CMD] L=live D=dry W=wet C=check R=reset H=help");
  }
}

// ═══════════════════════════════════════════════════════════════
// BITMAPS
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

const unsigned char bmp_open_mouth[] PROGMEM = {
  0x00,0x0f,0xf0,0x00,0x00,0x3f,0xfc,0x00,0x00,0x70,0x0e,0x00,0x00,0xe0,0x07,0x00,
  0x01,0xc0,0x03,0x80,0x03,0x80,0x01,0xc0,0x03,0x00,0x00,0xc0,0x07,0x3c,0x3c,0xe0,
  0x06,0x7e,0x7e,0x60,0x0e,0x7e,0x7e,0x70,0x0c,0x3c,0x3c,0x30,0x1c,0x00,0x00,0x38,
  0x18,0x03,0xc0,0x18,0x18,0x0c,0x30,0x18,0x18,0x10,0x08,0x18,0x18,0x10,0x08,0x18,
  0x18,0x10,0x08,0x18,0x1c,0x0c,0x30,0x38,0x0c,0x03,0xc0,0x30,0x0e,0x00,0x00,0x70,
  0x07,0x00,0x00,0xe0,0x03,0x80,0x01,0xc0,0x03,0xc0,0x03,0xc0,0x01,0xe0,0x07,0x80,
  0x00,0xf0,0x0f,0x00,0x00,0x78,0x1e,0x00,0x00,0x3f,0xfc,0x00,0x00,0x0f,0xf0,0x00
};

const unsigned char bmp_error[] PROGMEM = {
  0x00,0x0f,0xf0,0x00,0x00,0x3f,0xfc,0x00,0x00,0x70,0x0e,0x00,0x00,0xe0,0x07,0x00,
  0x01,0xc0,0x03,0x80,0x03,0x80,0x01,0xc0,0x03,0x00,0x00,0xc0,0x07,0x00,0x00,0xe0,
  0x06,0x00,0x00,0x60,0x0e,0x24,0x24,0x70,0x0c,0x18,0x18,0x30,0x1c,0x18,0x18,0x38,
  0x18,0x24,0x24,0x18,0x18,0x00,0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x0f,0xf0,0x18,
  0x18,0x00,0x00,0x18,0x1c,0x00,0x00,0x38,0x0c,0x00,0x00,0x30,0x0e,0x00,0x00,0x70,
  0x07,0x00,0x00,0xe0,0x03,0x80,0x01,0xc0,0x03,0xc0,0x03,0xc0,0x01,0xe0,0x07,0x80,
  0x00,0xf0,0x0f,0x00,0x00,0x78,0x1e,0x00,0x00,0x3f,0xfc,0x00,0x00,0x0f,0xf0,0x00
};

// ═══════════════════════════════════════════════════════════════
// OLED
// ═══════════════════════════════════════════════════════════════

void drawFooter(const char* text) {
  display.setTextSize(1);
  int cx = (128 - (int)strlen(text) * 6) / 2;
  if (cx < 0) cx = 0;
  display.setCursor(cx, 56);
  display.print(text);
}

void drawTopInfo() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (sensorOk) {
    display.print("M:"); display.print((int)soilPct); display.print("% ");
  } else {
    display.print("M:ERR ");
  }
  display.print(motorOn ? "ON" : "OFF");
  display.setCursor(84, 0);
  display.print("G"); display.print(SOIL_PIN);
  display.setCursor(0, 9);
  display.print("r:"); display.print(lastSoilRaw);
  display.print(" T:"); display.print((int)tempC); display.print("C");
}

void animateOptimalFrame(bool up) {
  display.clearDisplay(); drawTopInfo();
  display.drawBitmap(48, up ? 18 : 22, bmp_happy, 32, 32, SSD1306_WHITE);
  drawFooter("HAPPY"); display.display();
}

void animateDryFrame(bool left) {
  display.clearDisplay(); drawTopInfo();
  display.drawBitmap(left ? 46 : 50, 20, bmp_sad, 32, 32, SSD1306_WHITE);
  drawFooter("THIRSTY"); display.display();
}

void animateWateredFrame(bool dh) {
  display.clearDisplay(); drawTopInfo();
  display.drawBitmap(48, 20, bmp_open_mouth, 32, 32, SSD1306_WHITE);
  display.fillCircle(64, dh ? 12 : 18, 2, SSD1306_WHITE);
  drawFooter("DRINKING"); display.display();
}

void animateErrorFrame(bool blink) {
  display.clearDisplay(); drawTopInfo();
  if (blink) display.drawBitmap(48, 20, bmp_error, 32, 32, SSD1306_WHITE);
  drawFooter("CHECK SENSOR"); display.display();
}

void showBootMsg(const char* l1, const char* l2 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(8, 10); display.print("SMARTPLANT v3.5");
  display.setCursor(8, 26); display.print(l1);
  if (l2[0]) { display.setCursor(8, 42); display.print(l2); }
  display.display();
}

bool initOled() {
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) return true;
  if (display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) return true;
  return false;
}

// ═══════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  // RELAY OFF
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  motorOn = false;

  Serial.begin(115200);
  delay(500);
  Serial.println("\n================================");
  Serial.println("  SmartPlant v3.5 (final)");
  Serial.println("================================");

  esp_reset_reason_t r = esp_reset_reason();
  if (r == ESP_RST_PANIC) Serial.println("[BOOT] *** Previous boot CRASHED ***");
  else if (r == ESP_RST_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_INT_WDT)
    Serial.println("[BOOT] *** Previous boot WDT ***");
  else Serial.println("[BOOT] Normal start");

  dht.begin();
  Wire.begin(21, 22);
  if (!initOled()) Serial.println("[ERR] OLED fail");
  showBootMsg("Booting...");
  delay(300);

  prefs.begin("smartplant", false);
  loadPersistedState();
  loadCalibration();

  // Test GPIO32
  showBootMsg("Testing G32...");
  delay(200);
  uint32_t testVal = 0;
  for (int i = 0; i < 10; i++) {
    testVal += analogRead(SOIL_PIN);
    delay(10);
  }
  testVal /= 10;
  Serial.print("[BOOT] GPIO32 = "); Serial.println(testVal);

  if (testVal >= SENSOR_MIN_VALID) {
    sensorOk = true;
    showBootMsg("Sensor OK!");
    delay(500);
    for (int i = 0; i < RING_SIZE; i++) {
      ringBuf[i] = analogRead(SOIL_PIN);
      delay(10);
    }
    ringFull = true; ringIdx = 0;
    soilPct = readMoisturePercent();
  } else {
    sensorOk = false;
    Serial.println("[BOOT] *** No signal on GPIO32 ***");
    Serial.println("[BOOT] Check: Sensor AO -> GPIO32");
    showBootMsg("NO SENSOR", "AO -> GPIO32");
    delay(2000);
  }

  delay(500);
  readDHT();

  showBootMsg("WiFi...");
  connectWiFi();
  if (sensorOk) postSensorAndReadDecision();
  else safeRelayOff();

  currentState = decideState();
  persistState(true);
  digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);

  Serial.println("\n[CMD] L=live D=dry W=wet C=check R=reset H=help");
  Serial.println("[BOOT] Ready.\n");
}

// ═══════════════════════════════════════════════════════════════
// LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  handleSerialCal();

  if (millis() - lastSendMs >= SEND_INTERVAL_MS) {
    lastSendMs = millis();
    postSensorAndReadDecision();
    currentState = decideState();
  }

  switch (currentState) {
    case STATE_OPTIMAL:
      animateOptimalFrame(false); delay(220);
      animateOptimalFrame(true);  delay(220); break;
    case STATE_DRY:
      animateDryFrame(true);  delay(120);
      animateDryFrame(false); delay(120); break;
    case STATE_WATERED:
      animateWateredFrame(true);  delay(150);
      animateWateredFrame(false); delay(150); break;
    case STATE_ERROR:
      animateErrorFrame(true);  delay(500);
      animateErrorFrame(false); delay(500); break;
  }
  yield();
}