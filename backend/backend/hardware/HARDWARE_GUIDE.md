# SmartPlant Hardware Guide

This single guide combines ESP API details and hardware integration steps.

## Section 1: ESP to Backend API

### Endpoint

- `POST /api/esp/sensor`
- Header: `X-Device-Key: <your-device-key>` (required when backend key is configured)

### JSON Body Example

```json
{
  "zone_id": "A",
  "soil_moisture": 28.4,
  "temperature_c": 29.1,
  "humidity": 68,
  "rainfall_mm": 0,
  "sunlight_hours": 7,
  "wind_speed_kmh": 3
}
```

### Response Example

```json
{
  "zone_id": "A",
  "soil_moisture": 28.4,
  "prediction": "High",
  "motor_on": true,
  "reason": "auto:High|soil:28.4",
  "manual_override_active": false,
  "manual_override_until": null
}
```

### Manual + Auto Motor Behavior

- Dashboard `Toggle Motor` is supported.
- A manual toggle creates a temporary manual override window (120 seconds).
- During this window, ESP still uploads live sensor values, but backend keeps motor in manual state.
- After override expires, automatic model-based control resumes.

## Section 2: Security Setup

Set on backend environment:
- `SMARTPLANT_ESP_DEVICE_KEY=your-strong-device-key`

Use same key in firmware header:

```cpp
http.addHeader("X-Device-Key", "your-strong-device-key");
```

## Section 3: ESP32 Starter Firmware

```cpp
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASS";
const char* API_URL = "http://192.168.1.100:5000/api/esp/sensor";
const char* DEVICE_KEY = "your-strong-device-key";

const int SOIL_PIN = 34;
const int RELAY_PIN = 26;
const char* ZONE_ID = "A";

unsigned long lastSendMs = 0;
const unsigned long SEND_INTERVAL = 5000;

float mapMoisturePercent(int rawValue) {
  const int DRY = 3200;
  const int WET = 1400;
  float pct = (float)(DRY - rawValue) * 100.0f / (float)(DRY - WET);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

void connectWiFi() {
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH);
  connectWiFi();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    connectWiFi();
  }

  if (millis() - lastSendMs >= SEND_INTERVAL) {
    lastSendMs = millis();

    int rawSoil = analogRead(SOIL_PIN);
    float soilPct = mapMoisturePercent(rawSoil);

    HTTPClient http;
    http.begin(API_URL);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Key", DEVICE_KEY);

    StaticJsonDocument<256> doc;
    doc["zone_id"] = ZONE_ID;
    doc["soil_moisture"] = soilPct;
    doc["temperature_c"] = 28.0;
    doc["humidity"] = 65.0;
    doc["rainfall_mm"] = 0.0;
    doc["sunlight_hours"] = 7.0;
    doc["wind_speed_kmh"] = 2.0;

    String payload;
    serializeJson(doc, payload);

    int code = http.POST(payload);
    if (code > 0) {
      String resp = http.getString();
      StaticJsonDocument<256> out;
      auto err = deserializeJson(out, resp);
      if (!err) {
        bool motorOn = out["motor_on"] | false;
        digitalWrite(RELAY_PIN, motorOn ? LOW : HIGH);
      }
      Serial.println(resp);
    } else {
      Serial.printf("HTTP error: %d\n", code);
    }
    http.end();
  }
}
```

## Section 4: Integration Order

1. Sensor only test: send moisture every 5 seconds and verify dashboard updates.
2. Auto mode test: dry/wet changes should update prediction and motor state.
3. Manual override test: press dashboard `Toggle Motor`; verify motor follows manual state for ~120s.
4. Relay dry run: relay connected, pump disconnected.
5. Pump integration: verify safe ON/OFF behavior.
6. Calibration pass: tune dry/wet mapping values and thresholds.
7. Stability test: run for 2 to 4 hours.

## Section 5: ESP8266 Differences

- Replace `#include <WiFi.h>` with `#include <ESP8266WiFi.h>`.
- Use board-appropriate analog pin (usually `A0`).
- Keep endpoint and JSON shape unchanged.

## Section 6: Electrical Safety

- Use common GND between ESP and relay board.
- Use external motor/pump power.
- Most relay modules are active-low.
- Test with pump disconnected before full run.
