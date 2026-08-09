#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPIFFS.h>
#include <DHT.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <time.h>

// ==================== Connection constants ====================
const char* WIFI_SSID = "Infinix NOTE 40";
const char* WIFI_PASSWORD = "Achi@234";
// Realtime Database root, NO trailing slash. The REST helpers append the node
// path plus ".json" (e.g. FIREBASE_DB_URL + "/sensors/live.json"), matching the
// Firebase RTDB REST API. A trailing slash here would produce a double slash and
// 404 every request.
const char* FIREBASE_DB_URL = "https://hydrophonic-bucket-system-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* GSM_PHONE_NUMBER = "+94740879724";

// ==================== Firebase (REST over HTTPS) ====================
// This project talks to the Realtime Database with the ESP32 core's own
// WiFiClientSecure + HTTPClient, hitting the RTDB REST API directly, rather than
// the mobizt Firebase-ESP32 library. The library's BearSSL layer intermittently
// failed to initialize ("Failed to initialize the SSL layer") on this board;
// setInsecure() REST calls are lighter, need no ~40KB contiguous handshake block,
// and mirror the reference project's connection pattern.
//
// Forward declarations — the helpers are defined just above setupFirebase().
int    firebasePut(const char *path, const String &jsonBody);
int    firebasePost(const char *path, const String &jsonBody);
int    firebaseDelete(const char *path);
String firebaseGet(const char *path);
String firebaseGetQuery(const char *path, const String &query);

unsigned long lastFirebasePushMs = 0;
unsigned long lastFirebaseHistoryMs = 0;
unsigned long lastFirebaseControlMs = 0;
unsigned long lastHeapLogMs = 0;
unsigned long lastPruneMs = 0;

// History retention. A row every 60s is ~1440/day and nothing removed them, so
// the node grew forever. Keep a rolling window and delete in small batches.
// Rate matters: the batch/interval pair must exceed 1440 deletions/day or the
// backlog grows forever. 10 rows every 5 min = 2880/day, ~2x headroom, and keeps
// the query response near 2KB so it fits the session's buffer.
#define HISTORY_RETENTION_DAYS     30
#define HISTORY_PRUNE_BATCH        10      // rows per pass (also sizes an array)
const unsigned long HISTORY_PRUNE_INTERVAL_MS = 300000UL;  // 5 minutes

void pruneFirebaseHistory();   // defined below updateFirebase, called from it

// Commands are {state, ts} objects. Tracking the ts we last acted on means a
// command applies exactly once, repeat presses still register (new ts), and a
// stale command left in the database cannot restart a pump after a reboot.
double lastAppliedPumpTs      = 0;
double lastAppliedAutoPumpTs  = 0;
double lastAppliedWaterPumpTs = 0;
bool   controlsArmed          = false;  // true once boot-safe defaults are written

// ==================== WiFi ====================
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// ==================== Time (NTP) ====================
// millis() is uptime, not wall clock. History rows need real epoch time or the
// dashboard renders every point in 1970 and the axis resets on each reboot.
const char* NTP_SERVER_1 = "pool.ntp.org";
const char* NTP_SERVER_2 = "time.google.com";
const long  GMT_OFFSET_SEC = 5 * 3600 + 30 * 60;  // Asia/Colombo, UTC+05:30
const int   DAYLIGHT_OFFSET_SEC = 0;
bool timeSynced = false;

// ==================== GPIO Pin Assignments ====================
#define DHT_PIN 4            // DHT22 sensor
#define ONEWIRE_PIN 5        // DS18B20 water temperature
#define FLOW_SENSOR_PIN 18   // Water flow sensor
#define FLOAT_SWITCH_PIN 19  // Float switch
#define TDS_PIN 34           // TDS analog input
#define PUMP_PIN 25          // Peristaltic pump relay
#define WATER_PUMP_PIN 26    // Water pump relay
#define GSM_RX_PIN 16        // GSM module RX pin (to module TX)
#define GSM_TX_PIN 17        // GSM module TX pin (to module RX)
#define GSM_BAUD 9600
#define DEFAULT_GSM_PHONE GSM_PHONE_NUMBER
#define DEFAULT_GSM_MESSAGE "Water level is LOW"
#define I2C_SDA 21           // I2C Data line for LCD
#define I2C_SCL 22           // I2C Clock line for LCD

// ==================== EC Auto Pump Control ====================
const float EC_LOWER = 2.2f;         // Pump turns ON when EC drops below this
const float EC_UPPER = 2.8f;         // Pump turns OFF when EC reaches this
// A disconnected or dry TDS probe reads ~0 mS/cm, which is below EC_LOWER and
// would make auto mode dose nutrient forever into a tank it cannot actually
// measure. Treat any reading at or below this as a failed sensor, not a low tank.
const float EC_SENSOR_MIN_VALID = 0.05f;
const unsigned long MIN_PUMP_ON_MS = 10000;  // Minimum pump run time (10s)
const unsigned long MIN_PUMP_OFF_MS = 10000; // Minimum pump off time (10s)
const unsigned long MANUAL_OVERRIDE_TIMEOUT_MS = 300000; // Manual override timeout (5 min)

// The water pump has no EC feedback loop to switch it off again, so a remote
// "on" would otherwise run until someone noticed. Cap it.
const unsigned long WATER_PUMP_MAX_ON_MS = 30UL * 60UL * 1000UL;  // 30 minutes
unsigned long waterPumpOnSinceMs = 0;

const bool RELAY_ACTIVE_LOW = true;   // Most relay boards are active LOW
const float FLOW_PULSES_PER_LITER = 450.0f; // Approx. YF-S201: 7.5 pulses/sec = 1 L/min => 450 pulses/L
const float ADC_REF_VOLTAGE = 3.3f;
const int ADC_MAX = 4095;

// ==================== Sensor Objects ====================
DHT dht(DHT_PIN, DHT22);
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature tempSensor(&oneWire);
AsyncWebServer server(80);
LiquidCrystal_I2C lcd(0x27, 16, 4);  // I2C address 0x27, 16 columns, 4 rows
bool lcdAvailable = false;
HardwareSerial sim900(2);

// ==================== Global Variables ====================
volatile unsigned long flowPulseCount = 0;
unsigned long lastFlowCalcMs = 0;
float flowRateMlMin = 0.0f;
bool pumpActive = false;
bool waterPumpActive = false;
bool autoPumpEnabled = true;  // Auto mode on by default
bool pumpManualOverride = false;
unsigned long pumpManualOverrideTimeout = 0;
unsigned long lastPumpChange = 0;
unsigned long lastPumpOnTime = 0;
unsigned long lastPumpOffTime = 0;
bool pumpWasTurnedOn = false;  // Track if pump was just turned on automatically
bool waterLevelHigh = false;
bool lastWaterLevelHigh = true;
bool gsmEnabled = true;
String gsmPhoneNumber = DEFAULT_GSM_PHONE;
String gsmAlertMessage = DEFAULT_GSM_MESSAGE;
bool gsmInitialized = false;
bool gsmAlertPending = false;
unsigned long lastGsmAlertAttemptAt = 0;
const unsigned long GSM_ALERT_RETRY_MS = 30000;
Preferences gsmPrefs;
unsigned long lastLcdUpdate = 0;
const unsigned long LCD_UPDATE_INTERVAL = 5000;  // Update every 5 seconds

struct SensorData {
  float dhtTemp;
  float dhtHumidity;
  float waterTemp;
  float flowRateMlMin;
  bool waterLevelHigh;
  float tdsPPM;
  float tdsMScm;
  unsigned long timestamp;
};

void IRAM_ATTR flowISR() {
  flowPulseCount++;
}

// ==================== Relay Helpers ====================
void setRelayState(uint8_t pin, bool turnOn) {
  digitalWrite(pin, (turnOn ^ RELAY_ACTIVE_LOW) ? HIGH : LOW);
}

void setPump(bool turnOn) {
  pumpActive = turnOn;
  setRelayState(PUMP_PIN, turnOn);
  unsigned long now = millis();
  if (turnOn) {
    lastPumpOnTime = now;
    Serial.printf("[PUMP ON] Auto: %s | Manual Override: %s\n", 
                  autoPumpEnabled ? "enabled" : "disabled", 
                  pumpManualOverride ? "active" : "inactive");
  } else {
    lastPumpOffTime = now;
    Serial.printf("[PUMP OFF] Auto: %s | Manual Override: %s\n", 
                  autoPumpEnabled ? "enabled" : "disabled", 
                  pumpManualOverride ? "active" : "inactive");
  }
}

void setWaterPump(bool turnOn) {
  waterPumpActive = turnOn;
  setRelayState(WATER_PUMP_PIN, turnOn);
  if (turnOn) {
    waterPumpOnSinceMs = millis();
    Serial.println("[WATER PUMP ON]");
  } else {
    waterPumpOnSinceMs = 0;
    Serial.println("[WATER PUMP OFF]");
  }
}

void setAutoPump(bool enabled) {
  autoPumpEnabled = enabled;
  Serial.printf("Auto Pump %s\n", enabled ? "ENABLED" : "DISABLED");
  if (!enabled && pumpActive) {
    // Turn off pump immediately when auto mode is disabled
    setPump(false);
  }
}

String readGsmResponse(unsigned long timeoutMs) {
  unsigned long start = millis();
  String response;
  while (millis() - start < timeoutMs) {
    while (sim900.available()) {
      char c = sim900.read();
      response += c;
    }
    delay(10);
  }
  response.trim();
  if (response.length() > 0) {
    Serial.println(response);
  }
  return response;
}

void pollGsmOutput() {
  while (sim900.available()) {
    String response = sim900.readStringUntil('\n');
    response.trim();
    if (response.length() > 0) {
      Serial.println(response);
    }
  }
}

bool initGsmModule() {
  sim900.end();
  delay(200);
  sim900.begin(GSM_BAUD, SERIAL_8N1, GSM_RX_PIN, GSM_TX_PIN);
  delay(5000);

  while (sim900.available()) {
    sim900.read();
  }

  sim900.println("AT");
  String atResponse = readGsmResponse(2000);
  if (atResponse.indexOf("OK") == -1) {
    gsmInitialized = false;
    Serial.println("[GSM] Modem did not answer AT");
    return false;
  }

  sim900.println("AT+CMGF=1");
  readGsmResponse(1000);

  sim900.println("AT+CNMI=2,2,0,0,0");
  readGsmResponse(1000);

  gsmInitialized = true;
  Serial.println("[GSM] Modem ready");
  return true;
}

bool sendGsmSms(const String &to, const String &message) {
  if (to.length() < 5) {
    Serial.println("[GSM] Phone number is invalid");
    return false;
  }

  if (!initGsmModule()) {
    return false;
  }

  Serial.println("[GSM] Sending SMS...");
  sim900.println("AT+CMGF=1");
  delay(1000);
  readGsmResponse(1000);

  sim900.print("AT+CMGS=\"");
  sim900.print(to);
  sim900.println("\"");
  delay(2000);
  readGsmResponse(2000);

  sim900.print(message);
  delay(500);
  sim900.write(26);
  delay(5000);

  String sendResponse = readGsmResponse(3000);
  if (sendResponse.indexOf("OK") != -1 || sendResponse.indexOf("+CMGS:") != -1) {
    Serial.printf("[GSM] SMS sent to %s\n", to.c_str());
    return true;
  }

  Serial.println("[GSM] No confirmation from modem after SMS send");
  gsmInitialized = false;
  return false;
}

void loadGsmSettings() {
  gsmPrefs.begin("gsm", false);
  gsmEnabled = gsmPrefs.getBool("enabled", true);
  gsmPhoneNumber = gsmPrefs.getString("phone", DEFAULT_GSM_PHONE);
  if (gsmPhoneNumber.length() < 5) {
    gsmPhoneNumber = DEFAULT_GSM_PHONE;
  }
  gsmAlertMessage = gsmPrefs.getString("msg", DEFAULT_GSM_MESSAGE);
  if (gsmAlertMessage.length() == 0) {
    gsmAlertMessage = DEFAULT_GSM_MESSAGE;
  }
  Serial.printf("[GSM] Settings loaded: enabled=%s, phone=%s\n", gsmEnabled ? "true" : "false", gsmPhoneNumber.c_str());
}

void saveGsmSettings() {
  gsmPrefs.putBool("enabled", gsmEnabled);
  gsmPrefs.putString("phone", gsmPhoneNumber);
  gsmPrefs.putString("msg", gsmAlertMessage);
  Serial.println("[GSM] Settings saved");
}

void checkFloatSwitchAlert(bool currentWaterLevelHigh) {
  if (!gsmEnabled || gsmPhoneNumber.length() < 5) {
    gsmAlertPending = false;
    lastWaterLevelHigh = currentWaterLevelHigh;
    return;
  }

  if (currentWaterLevelHigh) {
    gsmAlertPending = false;
    lastWaterLevelHigh = true;
    return;
  }

  if (lastWaterLevelHigh) {
    gsmAlertPending = true;
    lastGsmAlertAttemptAt = 0;
    Serial.println("[GSM] Float switch LOW detected, sending alert...");
  }

  unsigned long now = millis();
  if (gsmAlertPending && (lastGsmAlertAttemptAt == 0 || now - lastGsmAlertAttemptAt >= GSM_ALERT_RETRY_MS)) {
    lastGsmAlertAttemptAt = now;
    if (sendGsmSms(gsmPhoneNumber, gsmAlertMessage)) {
      gsmAlertPending = false;
    } else {
      Serial.println("[GSM] Failed to send float LOW alert");
      Serial.println("[GSM] Will retry while float switch remains LOW");
    }
  }
  lastWaterLevelHigh = currentWaterLevelHigh;
}

// ==================== Sensor Reading ====================
// Raw ADC count from the last TDS read, kept for diagnostics. A submerged probe
// reading 0 counts means no signal is reaching TDS_PIN at all (wiring/power),
// as opposed to a real but badly-calibrated voltage.
int lastTdsRawADC = 0;

float readTdsPPM() {
  const int samples = 10;
  long total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogRead(TDS_PIN);
    delayMicroseconds(200);   // was delay(5): 50ms of blocking per read
  }
  float rawADC = total / (float)samples;
  lastTdsRawADC = (int)rawADC;
  float voltage = (rawADC / ADC_MAX) * ADC_REF_VOLTAGE;

  // Simple approximate formula. Calibrate with a known solution later.
  float tdsPPM = (voltage / 2.3f) * 1000.0f;
  if (tdsPPM < 0) tdsPPM = 0;
  return tdsPPM;
}

float readFlowMlMin() {
  unsigned long now = millis();
  unsigned long elapsed = now - lastFlowCalcMs;
  if (elapsed >= 1000) {
    noInterrupts();
    unsigned long pulses = flowPulseCount;
    flowPulseCount = 0;
    interrupts();

    // liters per minute = pulses * 60000 / (elapsed_ms * pulses_per_liter)
    float litersPerMinute = (pulses * 60000.0f) / (elapsed * FLOW_PULSES_PER_LITER);
    flowRateMlMin = litersPerMinute * 1000.0f;
    lastFlowCalcMs = now;
  }
  return flowRateMlMin;
}

// Single cached snapshot of sensor state. Everything (web API, LCD, control loop,
// Firebase) reads this instead of hitting the hardware itself.
//
// Why: readAllSensors() used to be called three times per loop pass — from the
// control loop, the LCD, and every /api/sensors request. The DHT22 is rated for
// one reading every 2s; polling it every 50ms makes it self-heat and drift, which
// is what produced impossible values like 74.7 C at 10.6% RH. The DS18B20 also
// blocks ~750ms per conversion at 12-bit, so three calls a pass stalled the loop.
// -1 marks "no valid reading yet"; the dashboard renders that as "--".
SensorData latestData{ -1, -1, -1, 0, false, 0, 0, 0 };

const unsigned long DHT_MIN_INTERVAL_MS = 2000;   // datasheet minimum for DHT22
const unsigned long DS18B20_INTERVAL_MS = 2000;   // conversion is slow; no need to rush
const unsigned long TDS_INTERVAL_MS     = 1000;

// Refresh the cache. Each sensor is rate-limited independently and keeps its last
// good value in between, so calling this every loop pass is cheap.
void updateSensorCache() {
  unsigned long now = millis();
  latestData.timestamp = now;

  static unsigned long lastDhtMs = 0;
  if (lastDhtMs == 0 || now - lastDhtMs >= DHT_MIN_INTERVAL_MS) {
    lastDhtMs = now;
    float airTemp  = dht.readTemperature();
    float humidity = dht.readHumidity();
    // Hold the last good value on a NaN (transient CRC/timing failure) rather
    // than flapping the display to "--". Both start at -1, so they stay -1 until
    // the first successful read.
    if (!isnan(airTemp))  latestData.dhtTemp     = airTemp;
    if (!isnan(humidity)) latestData.dhtHumidity = humidity;
  }

  // Non-blocking pattern: read whatever the previous request produced, then kick
  // off the next conversion. setWaitForConversion(false) in setup() makes
  // requestTemperatures() return immediately instead of blocking ~750ms.
  static unsigned long lastDsMs = 0;
  if (lastDsMs == 0 || now - lastDsMs >= DS18B20_INTERVAL_MS) {
    lastDsMs = now;
    float waterTemp = tempSensor.getTempCByIndex(0);
    latestData.waterTemp = (waterTemp == 85.0f || waterTemp == -127.0f) ? -1 : waterTemp;
    tempSensor.requestTemperatures();
  }

  static unsigned long lastTdsMs = 0;
  if (lastTdsMs == 0 || now - lastTdsMs >= TDS_INTERVAL_MS) {
    lastTdsMs = now;
    latestData.tdsPPM  = readTdsPPM();
    latestData.tdsMScm = latestData.tdsPPM / 640.0f;
  }

  // Cheap reads, safe every pass.
  // Float switch with INPUT_PULLUP: HIGH means water level is high, LOW means low
  latestData.waterLevelHigh = (digitalRead(FLOAT_SWITCH_PIN) == HIGH);
  waterLevelHigh = latestData.waterLevelHigh;
  latestData.flowRateMlMin = readFlowMlMin();
}

String buildSensorJson() {
  // Uses the cache, not a fresh hardware read: this runs inside an async web
  // server callback, where a blocking sensor read can trip the task watchdog.
  const SensorData &data = latestData;
  JsonDocument doc;
  doc["dhtTemperature"] = data.dhtTemp;
  doc["dhtHumidity"] = data.dhtHumidity;
  doc["waterTemperature"] = data.waterTemp;
  // L/min, matching the Firebase payload and the dashboard's label. This used
  // to emit mL/min here while Firebase sent L/min, so local mode read 1000x high.
  doc["flowRate"] = data.flowRateMlMin / 1000.0f;
  doc["flowRateMlMin"] = data.flowRateMlMin;
  doc["waterLevel"] = data.waterLevelHigh ? "HIGH" : "LOW";
  doc["waterLevelValue"] = data.waterLevelHigh ? 1 : 0;
  doc["tdsPPM"] = data.tdsPPM;
  doc["tdsMScm"] = data.tdsMScm;
  doc["pumpActive"] = pumpActive;
  doc["waterPumpActive"] = waterPumpActive;
  doc["autoPumpEnabled"] = autoPumpEnabled;
  doc["pumpManualOverride"] = pumpManualOverride;
  doc["ecLower"] = EC_LOWER;
  doc["ecUpper"] = EC_UPPER;
  doc["timestamp"] = data.timestamp;

  String response;
  serializeJson(doc, response);
  return response;
}

void sendJson(AsyncWebServerRequest *request, int code, const String &payload) {
  AsyncWebServerResponse *response = request->beginResponse(code, "application/json", payload);
  response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  request->send(response);
}

void sendStatusJson(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["status"] = "ok";
  doc["pump"] = pumpActive ? "on" : "off";
  doc["waterPump"] = waterPumpActive ? "on" : "off";
  // The dashboard's checkStatus() reads waterPumpActive; emitting only
  // "waterPump" here meant the water pump always displayed OFF.
  doc["waterPumpActive"] = waterPumpActive ? "on" : "off";
  doc["autoPump"] = autoPumpEnabled ? "enabled" : "disabled";
  doc["pumpManualOverride"] = pumpManualOverride ? "active" : "inactive";
  doc["waterLevel"] = waterLevelHigh ? "HIGH" : "LOW";
  String response;
  serializeJson(doc, response);
  sendJson(request, 200, response);
}

// ==================== LCD Display ====================
void setupLCD() {
  Wire.begin(I2C_SDA, I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Hydroponic System");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
  lcd.setCursor(0, 2);
  lcd.print("Please wait...");
  lcd.setCursor(0, 3);
  lcd.print(" ");
  lcdAvailable = true;
  Serial.println("LCD initialized successfully");
}

void updateLCDDisplay() {
  if (!lcdAvailable) return;
  
  unsigned long now = millis();
  if (now - lastLcdUpdate < LCD_UPDATE_INTERVAL) return;
  lastLcdUpdate = now;
  
  SensorData data = latestData;

  char row1[17] = "                ";
  char row2[17] = "                ";
  char row3[17] = "                ";
  char row4[17] = "                ";

  snprintf(row1, sizeof(row1), "PUMP:%s  AUTO:%s", pumpActive ? "ON" : "OFF", autoPumpEnabled ? "ON" : "OFF");
  snprintf(row2, sizeof(row2), "EC:%4.2f W:%4.1fC", data.tdsMScm, data.waterTemp);
  snprintf(row3, sizeof(row3), "F:%4.1fLPM L:%s", data.flowRateMlMin / 1000.0f, data.waterLevelHigh ? "HIGH" : "LOW");
  snprintf(row4, sizeof(row4), "GSM:%s OV:%s", gsmEnabled ? "ON" : "OFF", pumpManualOverride ? "YES" : "NO");

  lcd.setCursor(0, 0);
  lcd.print("                ");
  lcd.setCursor(0, 0);
  lcd.print(row1);

  lcd.setCursor(0, 1);
  lcd.print("                ");
  lcd.setCursor(0, 1);
  lcd.print(row2);

  lcd.setCursor(0, 2);
  lcd.print("                ");
  lcd.setCursor(0, 2);
  lcd.print(row3);

  lcd.setCursor(0, 3);
  lcd.print("                ");
  lcd.setCursor(0, 3);
  lcd.print(row4);
}

bool getActionParam(AsyncWebServerRequest *request, String &action) {
  if (request->hasParam("action", true)) {
    action = request->getParam("action", true)->value();
  } else if (request->hasParam("action")) {
    action = request->getParam("action")->value();
  } else {
    return false;
  }
  
  action.toLowerCase();
  action.trim();
  return true;
}

bool getStringParam(AsyncWebServerRequest *request, const char *name, String &value) {
  if (request->hasParam(name, true)) {
    value = request->getParam(name, true)->value();
  } else if (request->hasParam(name)) {
    value = request->getParam(name)->value();
  } else {
    return false;
  }
  value.trim();
  return true;
}

// Normalize various action synonyms to canonical values: "enable" or "disable".
// Returns empty string when the action is not recognized.
String normalizeAction(const String &raw) {
  String s = raw;
  s.toLowerCase();
  s.trim();
  if (s.length() == 0) return String("");

  if (s.startsWith("e") || s == "enable" || s == "enabled" || s == "on" || s == "1" || s == "true") {
    return String("enable");
  }
  if (s.startsWith("d") || s == "disable" || s == "disabled" || s == "off" || s == "0" || s == "false") {
    return String("disable");
  }
  return String("");
}

// ==================== Web Server ====================
void setupWebServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists("/index.html")) request->send(SPIFFS, "/index.html", "text/html");
    else request->send(404, "text/plain", "index.html not found in SPIFFS");
  });

  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists("/style.css")) request->send(SPIFFS, "/style.css", "text/css");
    else request->send(404, "text/plain", "style.css not found in SPIFFS");
  });

  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (SPIFFS.exists("/script.js")) request->send(SPIFFS, "/script.js", "application/javascript");
    else request->send(404, "text/plain", "script.js not found in SPIFFS");
  });

  server.on("/api/sensors", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendJson(request, 200, buildSensorJson());
  });

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *request) {
    sendStatusJson(request);
  });

  server.on("/api/pump", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action;
    if (!getActionParam(request, action)) {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing action parameter\"}");
      return;
    }

    if (action == "on") {
      setPump(true);
      pumpManualOverride = true;
      pumpManualOverrideTimeout = millis() + MANUAL_OVERRIDE_TIMEOUT_MS;
    }
    else if (action == "off") {
      setPump(false);
      pumpManualOverride = true;
      pumpManualOverrideTimeout = millis() + MANUAL_OVERRIDE_TIMEOUT_MS;
    }
    else {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid action\"}");
      return;
    }
    sendStatusJson(request);
  });

  server.on("/api/pump/auto", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action;
    if (!getActionParam(request, action)) {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing action parameter\"}");
      return;
    }
    // Normalize action values to accepted canonical values
    String norm = normalizeAction(action);
    if (norm.length() == 0) {
      Serial.printf("[API] Invalid auto pump action received: '%s'\n", action.c_str());
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid action\",\"accepted\":[\"enable\",\"disable\"]}");
      return;
    }

    if (norm == "enable") {
      setAutoPump(true);
      pumpManualOverride = false;
    } else { // "disable"
      setAutoPump(false);
      pumpManualOverride = false;
    }
    sendStatusJson(request);
  });

  server.on("/api/waterpump", HTTP_POST, [](AsyncWebServerRequest *request) {
    String action;
    if (!getActionParam(request, action)) {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Missing action parameter\"}");
      return;
    }

    if (action == "on") {
      setWaterPump(true);
    } else if (action == "off") {
      setWaterPump(false);
    } else {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid action\"}");
      return;
    }

    JsonDocument doc;
    doc["status"] = "ok";
    doc["waterPump"] = waterPumpActive ? "on" : "off";
    String response;
    serializeJson(doc, response);
    sendJson(request, 200, response);
  });

  server.on("/api/gsm", HTTP_GET, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["status"] = "ok";
    doc["enabled"] = gsmEnabled;
    doc["phoneNumber"] = gsmPhoneNumber;
    doc["message"] = gsmAlertMessage;
    String response;
    serializeJson(doc, response);
    sendJson(request, 200, response);
  });

  server.on("/api/gsm", HTTP_POST, [](AsyncWebServerRequest *request) {
    String enabledValue;
    String phoneValue;
    String messageValue;

    if (getStringParam(request, "enabled", enabledValue)) {
      enabledValue.toLowerCase();
      gsmEnabled = (enabledValue == "1" || enabledValue == "true" || enabledValue == "yes");
    }
    if (getStringParam(request, "phoneNumber", phoneValue)) {
      gsmPhoneNumber = phoneValue;
    }
    if (getStringParam(request, "message", messageValue)) {
      gsmAlertMessage = messageValue;
    }

    saveGsmSettings();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["enabled"] = gsmEnabled;
    doc["phoneNumber"] = gsmPhoneNumber;
    doc["message"] = gsmAlertMessage;
    String response;
    serializeJson(doc, response);
    sendJson(request, 200, response);
  });

  server.on("/api/gsm/send", HTTP_POST, [](AsyncWebServerRequest *request) {
    JsonDocument doc;
    if (gsmPhoneNumber.length() < 5 || gsmAlertMessage.length() == 0) {
      doc["status"] = "error";
      doc["message"] = "GSM phone number or message not configured";
      String response;
      serializeJson(doc, response);
      sendJson(request, 400, response);
      return;
    }

    bool sent = sendGsmSms(gsmPhoneNumber, gsmAlertMessage);
    if (sent) {
      doc["status"] = "ok";
      doc["message"] = "SMS sent";
    } else {
      doc["status"] = "error";
      doc["message"] = "SMS send failed";
    }
    doc["enabled"] = gsmEnabled;
    doc["phoneNumber"] = gsmPhoneNumber;
    doc["messageText"] = gsmAlertMessage;
    String response;
    serializeJson(doc, response);
    sendJson(request, sent ? 200 : 500, response);
  });

  server.on("/api/testrelay", HTTP_GET, [](AsyncWebServerRequest *request) {
    setPump(true);
    delay(700);
    setPump(false);
    request->send(200, "application/json", "{\"status\":\"ok\",\"relay\":\"pump_pulsed\"}");
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  server.begin();
  Serial.println("Web server started");
}

// ==================== Setup Helpers ====================
void setupSPIFFS() {
  Serial.println("Mounting SPIFFS...");
  if (!SPIFFS.begin(true)) {
    Serial.println("ERROR: SPIFFS mount failed");
    return;
  }
  Serial.println("SPIFFS mounted");
}

void setupWiFi() {
  Serial.println("Connecting to WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print('.');
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    // Disable WiFi modem sleep. With it on (the default), the radio naps between
    // beacons and the BearSSL handshake to Firebase intermittently fails to send
    // its ClientHello — surfacing as "Error writing to basic client" and
    // "Failed to initialize the SSL layer". Keeping the radio awake costs a
    // little power but makes the TLS connection reliable.
    WiFi.setSleep(false);
  } else {
    Serial.println("\nWiFi failed. Starting Access Point mode.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("HydroponicDashboard", "hydroponic123");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }
}

void setupTime() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER_1, NTP_SERVER_2);
  Serial.print("[NTP] Syncing clock");
  struct tm timeinfo;
  for (int i = 0; i < 20 && !timeSynced; i++) {
    if (getLocalTime(&timeinfo, 500)) {
      timeSynced = true;
      break;
    }
    Serial.print('.');
  }
  if (timeSynced) {
    Serial.printf("\n[NTP] Synced: %04d-%02d-%02d %02d:%02d:%02d\n",
                  timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    Serial.println("\n[NTP] Sync failed; will retry in the background");
  }
}

// Epoch milliseconds, for timestamps the dashboard can plot directly.
// Returns 0 until the clock is actually valid, so callers can skip writing a
// point rather than logging one that renders in 1970.
double epochMillis() {
  time_t nowSec = time(nullptr);
  if (nowSec < 1700000000) return 0;   // still pre-sync
  timeSynced = true;
  return (double)nowSec * 1000.0;
}

void setupFirebase() {
  // Nothing to initialize for REST-over-HTTPS: each call opens its own
  // WiFiClientSecure with setInsecure(). We only confirm WiFi is up and log a
  // heap snapshot so a future memory regression is visible at the same spot the
  // old library's handshake block used to be measured.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[Firebase] WiFi not connected; REST calls will no-op until it is");
    return;
  }
  Serial.printf("[Firebase] Heap at connect: free=%u, maxAlloc=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  Serial.println("[Firebase] Using REST API (open rules, no sign-in)");

  // Reachability probe: GET a small fixed node (not the DB root — that would pull
  // the entire /sensors/history tree). Returns "null" on a fresh DB or a JSON
  // body otherwise; either proves the URL, DNS, and TLS all work, so a wrong DB
  // URL surfaces here at boot rather than as silent write failures later.
  String probe = firebaseGet("/sensors/live");
  Serial.printf("[Firebase] Connectivity probe (/sensors/live): %s\n",
                probe.length() ? probe.substring(0, 60).c_str() : "(no response)");
}

// ==================== Firebase REST Helpers ====================
// Each helper opens a short-lived TLS connection with certificate validation
// disabled (setInsecure). The database rules are open, so no auth token is sent.
// Returns the HTTP status code, or -1 when WiFi is down / the request never left.

// Builds the full REST URL: <db root> + <path> + ".json".
static String firebaseUrl(const char *path) {
  return String(FIREBASE_DB_URL) + String(path) + ".json";
}

// PUT overwrites the node at `path` wholesale — used for hot single-value state
// like /sensors/live and /controls/*, so the node never accumulates push keys.
int firebasePut(const char *path, const String &jsonBody) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, firebaseUrl(path))) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  int code = http.sendRequest("PUT", (uint8_t *)jsonBody.c_str(), jsonBody.length());
  http.end();
  return code;
}

// POST appends a child under `path` with a server-generated push key — used for
// /sensors/history so each sample becomes its own time-ordered row.
int firebasePost(const char *path, const String &jsonBody) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, firebaseUrl(path))) return -1;
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(8000);
  int code = http.POST(jsonBody);
  http.end();
  return code;
}

// DELETE removes the node at `path` — used to prune old history rows so the node
// does not grow without bound.
int firebaseDelete(const char *path) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, firebaseUrl(path))) return -1;
  http.setTimeout(8000);
  int code = http.sendRequest("DELETE");
  http.end();
  return code;
}

// GET returns the response body (JSON) as a String, or "" on any failure. For
// requests that need a query string, use firebaseGetQuery() below — the RTDB REST
// API requires the query to come after the ".json" suffix.
String firebaseGet(const char *path) {
  if (WiFi.status() != WL_CONNECTED) return "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, firebaseUrl(path))) return "";
  http.addHeader("Accept", "application/json");
  http.setTimeout(8000);
  int code = http.GET();
  String body = "";
  if (code == 200) body = http.getString();
  http.end();
  return body;
}

// GET variant for query strings. The RTDB REST API needs the query AFTER ".json"
// (e.g. .../history.json?orderBy="timestamp"&limitToFirst=10), so this builds the
// URL directly instead of going through firebaseUrl().
String firebaseGetQuery(const char *path, const String &query) {
  if (WiFi.status() != WL_CONNECTED) return "";
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = String(FIREBASE_DB_URL) + String(path) + ".json?" + query;
  if (!http.begin(client, url)) return "";
  http.addHeader("Accept", "application/json");
  http.setTimeout(8000);
  int code = http.GET();
  String body = "";
  if (code == 200) body = http.getString();
  http.end();
  return body;
}

void updateFirebase(const SensorData &data) {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();

  // Push live sensor state to Firebase every 5 seconds
  if (now - lastFirebasePushMs >= 5000) {
    lastFirebasePushMs = now;

    JsonDocument json;
    json["dhtTemperature"]    = data.dhtTemp;
    json["dhtHumidity"]       = data.dhtHumidity;
    json["waterTemperature"]  = data.waterTemp;
    json["flowRate"]          = data.flowRateMlMin / 1000.0f;
    json["waterLevel"]        = data.waterLevelHigh ? "HIGH" : "LOW";
    json["tdsPPM"]            = data.tdsPPM;
    json["tdsMScm"]           = data.tdsMScm;
    json["pumpActive"]        = pumpActive;
    json["waterPumpActive"]   = waterPumpActive;
    json["autoPumpEnabled"]   = autoPumpEnabled;
    json["pumpManualOverride"] = pumpManualOverride;
    // Thresholds travel with the payload so the dashboard shows the values the
    // device is actually using instead of falling back to hardcoded text.
    json["ecLower"]           = EC_LOWER;
    json["ecUpper"]           = EC_UPPER;
    // Epoch-ms is ~1.75e12: serialize as a 64-bit integer, not a double.
    // ArduinoJson renders large doubles in reduced precision (e.g. 1.75468e12),
    // which would round the timestamp by ~1e5 ms and break the dashboard's
    // 45-second freshness check and chart axis.
    json["timestamp"]         = (long long)epochMillis();

    String body;
    serializeJson(json, body);
    // PUT overwrites /sensors/live so the node stays a single hot record.
    int code = firebasePut("/sensors/live", body);
    if (code != 200) {
      Serial.printf("[Firebase ERROR] live push HTTP %d\n", code);
    }
  }

  // Push historical log entry every 60 seconds
  if (now - lastFirebaseHistoryMs >= 60000) {
    lastFirebaseHistoryMs = now;   // reset first, so a skip retries in 60s
                                   // rather than spinning every loop tick

    double ts = epochMillis();
    // Skip logging until the clock is real. A point written with uptime instead
    // of epoch time plots in 1970 and corrupts the chart's axis.
    if (ts == 0) {
      Serial.println("[Firebase] Skipping history row: clock not synced yet");
      return;
    }

    JsonDocument json;
    json["dhtTemperature"]   = data.dhtTemp;
    json["dhtHumidity"]      = data.dhtHumidity;
    json["waterTemperature"] = data.waterTemp;
    json["flowRate"]         = data.flowRateMlMin / 1000.0f;
    json["waterLevel"]       = data.waterLevelHigh ? "HIGH" : "LOW";
    json["waterLevelValue"]  = data.waterLevelHigh ? 1 : 0;
    json["tdsMScm"]          = data.tdsMScm;
    json["timestamp"]        = (long long)ts;   // integer epoch-ms, see live push

    String body;
    serializeJson(json, body);
    // POST appends a push-keyed child under /sensors/history.
    int code = firebasePost("/sensors/history", body);
    if (code != 200) {
      Serial.printf("[Firebase] History push HTTP %d\n", code);
    }
  }

  pruneFirebaseHistory();
}

// ==================== History Retention ====================
// A row is logged every 60s and nothing ever removed it, so /sensors/history grew
// without bound (~1440 rows/day, ~525k/year). That slowly inflates stored bytes
// and download quota for data nobody charts. Trim anything past the retention
// window, a few rows at a time so a delete pass never blocks the control loop.
void pruneFirebaseHistory() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastPruneMs < HISTORY_PRUNE_INTERVAL_MS) return;
  lastPruneMs = now;

  double nowMs = epochMillis();
  if (nowMs == 0) return;  // clock not synced; cutoff would be meaningless
  double cutoff = nowMs - (double)HISTORY_RETENTION_DAYS * 86400000.0;
  if (cutoff <= 0) return;

  // Oldest-first, capped batch via the RTDB REST query API. Needs
  // .indexOn:["timestamp"] in the rules, which database.rules.json declares.
  // orderBy value must be a quoted JSON string per the REST spec.
  String query = "orderBy=%22timestamp%22&endAt=" + String((long long)cutoff) +
                 "&limitToFirst=" + String(HISTORY_PRUNE_BATCH);
  String body = firebaseGetQuery("/sensors/history", query);
  if (body.length() == 0 || body == "null") return;  // nothing older than window

  // Parse the returned object: { "-Nxxx": {..., "timestamp": ...}, ... }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("[Prune] Parse failed: %s\n", err.c_str());
    return;
  }
  JsonObject obj = doc.as<JsonObject>();
  if (obj.isNull()) return;

  // Collect keys first, then delete. Every RTDB push key begins with '-', so a
  // stray sensor field could never be mistaken for a row key.
  String keys[HISTORY_PRUNE_BATCH];
  int found = 0;
  for (JsonPair kv : obj) {
    const char *k = kv.key().c_str();
    if (found < HISTORY_PRUNE_BATCH && k && k[0] == '-') {
      keys[found++] = String(k);
    }
  }

  int deleted = 0;
  for (int i = 0; i < found; i++) {
    String path = "/sensors/history/" + keys[i];
    if (firebaseDelete(path.c_str()) == 200) deleted++;
    else Serial.printf("[Prune] Delete failed for %s\n", keys[i].c_str());
  }

  if (deleted > 0) {
    Serial.printf("[Prune] Removed %d history rows older than %d days\n",
                  deleted, HISTORY_RETENTION_DAYS);
  }
}

// ==================== Remote Control via Firebase (Netlify → ESP32) ====================
// Commands are {state:"on"|"off", ts:<epoch ms>} objects rather than bare
// strings. Comparing the timestamp instead of the string value means:
//   - pressing the same button twice still registers (the ts changes), and
//   - a stale command sitting in the database cannot re-fire after a reboot,
//     because boot writes fresh "off" defaults before polling begins.

// Reads a {state, ts} command node. Returns false if absent or malformed.
bool readCommand(const char *path, String &state, double &ts) {
  String body = firebaseGet(path);
  if (body.length() == 0 || body == "null") return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  JsonVariant stateVar = doc["state"];
  JsonVariant tsVar     = doc["ts"];
  if (stateVar.isNull() || tsVar.isNull()) return false;

  state = String((const char *)(stateVar.as<const char *>() ? stateVar.as<const char *>() : ""));
  state.toLowerCase();
  state.trim();
  ts = tsVar.as<double>();
  return state.length() > 0;
}

bool writeCommand(const char *path, const char *state, double ts) {
  JsonDocument doc;
  doc["state"] = state;
  doc["ts"]    = (long long)ts;   // integer epoch-ms, avoids double rounding
  String body;
  serializeJson(doc, body);
  return firebasePut(path, body) == 200;
}

// Force both pumps off in the database before we start obeying it, so a command
// left over from before a power cut can never restart a pump unattended.
void armControls() {
  double ts = epochMillis();
  if (ts == 0) return;  // wait for a real clock so the ts ordering is meaningful

  // Backdate the boot stamp slightly. The watermark below is set from the ESP32's
  // NTP clock, but dashboard commands are stamped by Firebase's server clock, and
  // the two can disagree by a second or so. If NTP ran ahead, a button pressed
  // right after boot would carry an earlier ts and be silently ignored. Nudging
  // the stamp into the past closes that window and costs nothing: these writes
  // OVERWRITE whatever stale command was stored, so no old command survives to be
  // re-applied. Written value and watermark must stay equal, otherwise the device
  // re-reads its own "off" next poll and that would set pumpManualOverride,
  // suppressing auto-dosing after every boot.
  const double ARM_SKEW_MARGIN_MS = 10000.0;
  double armTs = ts - ARM_SKEW_MARGIN_MS;

  setPump(false);
  setWaterPump(false);

  bool ok = writeCommand("/controls/pump", "off", armTs);
  ok &= writeCommand("/controls/waterPump", "off", armTs);
  ok &= writeCommand("/controls/autoPump", autoPumpEnabled ? "enable" : "disable", armTs);

  if (!ok) {
    Serial.println("[Firebase] Boot arming failed: one or more control writes did not return 200");
    return;  // retry on the next poll rather than obeying stale commands
  }

  // Treat these as already applied so we don't immediately re-act on them.
  lastAppliedPumpTs = armTs;
  lastAppliedWaterPumpTs = armTs;
  lastAppliedAutoPumpTs = armTs;
  controlsArmed = true;
  Serial.println("[Firebase] Controls armed: pumps forced OFF at boot");
}

void pollFirebaseGsm() {
  String body = firebaseGet("/gsm");
  if (body.length() == 0 || body == "null") return;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return;

  bool changed = false;
  if (!doc["phoneNumber"].isNull()) {
    String phone = String((const char *)(doc["phoneNumber"].as<const char *>() ?
                                          doc["phoneNumber"].as<const char *>() : ""));
    phone.trim();
    if (phone.length() >= 5 && phone != gsmPhoneNumber) {
      gsmPhoneNumber = phone;
      changed = true;
    }
  }
  if (!doc["message"].isNull()) {
    String msg = String((const char *)(doc["message"].as<const char *>() ?
                                        doc["message"].as<const char *>() : ""));
    msg.trim();
    if (msg.length() > 0 && msg != gsmAlertMessage) {
      gsmAlertMessage = msg;
      changed = true;
    }
  }
  if (!doc["enabled"].isNull()) {
    bool en = doc["enabled"].as<bool>();
    if (en != gsmEnabled) {
      gsmEnabled = en;
      changed = true;
    }
  }

  if (changed) {
    saveGsmSettings();
    Serial.printf("[Firebase] GSM settings updated remotely: enabled=%s, phone=%s\n",
                  gsmEnabled ? "true" : "false", gsmPhoneNumber.c_str());
  }
}

void pollFirebaseControls() {
  if (WiFi.status() != WL_CONNECTED) return;

  unsigned long now = millis();
  if (now - lastFirebaseControlMs < 2000) return;  // Poll every 2 seconds
  lastFirebaseControlMs = now;

  if (!controlsArmed) {
    armControls();
    return;
  }

  String state;
  double ts = 0;

  // --- Air / dosing pump ---
  if (readCommand("/controls/pump", state, ts) && ts > lastAppliedPumpTs) {
    lastAppliedPumpTs = ts;
    if (state == "on" || state == "off") {
      setPump(state == "on");
      pumpManualOverride = true;
      pumpManualOverrideTimeout = millis() + MANUAL_OVERRIDE_TIMEOUT_MS;
      Serial.printf("[Firebase] Remote command: Air Pump %s\n", state.c_str());
    } else {
      Serial.printf("[Firebase] Ignoring invalid pump command: '%s'\n", state.c_str());
    }
  }

  // --- Auto pump mode ---
  if (readCommand("/controls/autoPump", state, ts) && ts > lastAppliedAutoPumpTs) {
    lastAppliedAutoPumpTs = ts;
    String norm = normalizeAction(state);
    if (norm.length() > 0) {
      setAutoPump(norm == "enable");
      pumpManualOverride = false;
      Serial.printf("[Firebase] Remote command: Auto Pump %s\n", norm.c_str());
    } else {
      Serial.printf("[Firebase] Ignoring invalid autoPump command: '%s'\n", state.c_str());
    }
  }

  // --- Water pump ---
  if (readCommand("/controls/waterPump", state, ts) && ts > lastAppliedWaterPumpTs) {
    lastAppliedWaterPumpTs = ts;
    if (state == "on" || state == "off") {
      setWaterPump(state == "on");
      Serial.printf("[Firebase] Remote command: Water Pump %s\n", state.c_str());
    } else {
      Serial.printf("[Firebase] Ignoring invalid waterPump command: '%s'\n", state.c_str());
    }
  }

  pollFirebaseGsm();
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nHydroponic Dashboard Starting...");

  pinMode(DHT_PIN, INPUT);
  pinMode(ONEWIRE_PIN, INPUT);
  pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
  pinMode(FLOAT_SWITCH_PIN, INPUT_PULLUP);
  pinMode(TDS_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(WATER_PUMP_PIN, OUTPUT);

  setPump(false);
  setWaterPump(false);

  loadGsmSettings();
  initGsmModule();

  dht.begin();
  tempSensor.begin();
  // Non-blocking conversions: requestTemperatures() returns immediately instead
  // of stalling the loop ~750ms while the DS18B20 converts. updateSensorCache()
  // reads the previous result before requesting the next one.
  tempSensor.setWaitForConversion(false);
  tempSensor.requestTemperatures();   // prime the first conversion
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowISR, RISING);
  lastFlowCalcMs = millis();

  setupLCD();
  setupSPIFFS();
  setupWiFi();
  setupTime();       // must precede Firebase: TLS validation and history rows need a real clock
  setupFirebase();
  setupWebServer();

  Serial.println("Setup complete. Open http://<ESP32_IP>");
}

void loop() {
  pollGsmOutput();
  unsigned long now = millis();
  
  // Check if manual override timeout has expired
  if (pumpManualOverride && pumpManualOverrideTimeout > 0 && now >= pumpManualOverrideTimeout) {
    pumpManualOverride = false;
    Serial.println("Manual pump override timeout expired. Auto control resumed.");
  }

  // Water pump safety cap. Nothing else ever switches this pump off, so without
  // this a remote "on" plus a dropped connection runs it indefinitely.
  if (waterPumpActive && waterPumpOnSinceMs > 0 &&
      now - waterPumpOnSinceMs >= WATER_PUMP_MAX_ON_MS) {
    Serial.println("[SAFETY] Water pump hit max runtime. Forcing OFF.");
    setWaterPump(false);
    if (WiFi.status() == WL_CONNECTED) {
      writeCommand("/controls/waterPump", "off", epochMillis());
      lastAppliedWaterPumpTs = epochMillis();
    }
  }
  
  // Refresh the sensor cache once per pass; every other consumer reads from it.
  updateSensorCache();
  SensorData data = latestData;
  checkFloatSwitchAlert(data.waterLevelHigh);
  updateFirebase(data);
  pollFirebaseControls();  // Handle remote commands from Netlify dashboard

  // Auto pump control logic (only if auto mode enabled and no manual override)
  if (autoPumpEnabled && !pumpManualOverride) {
    // Sensor sanity guard: a dead or disconnected TDS probe reads ~0.00, which
    // would otherwise look like an empty tank and dose nutrient forever. When the
    // reading is implausible, force the pump off (once) and skip dosing entirely;
    // the rest of loop() still runs so the LCD and logs keep updating.
    if (data.tdsMScm <= EC_SENSOR_MIN_VALID) {
      if (pumpActive) {
        Serial.printf("[SAFETY] EC reading %.2f mS/cm is implausible (probe disconnected?). Forcing pump OFF.\n", data.tdsMScm);
        setPump(false);
        pumpWasTurnedOn = false;
      }
    } else {
      // Turn pump ON if EC drops below lower threshold
      if (data.tdsMScm < EC_LOWER && !pumpActive) {
        if (lastPumpOffTime == 0 || (now - lastPumpOffTime >= MIN_PUMP_OFF_MS)) {
          Serial.printf("EC (%.2f mS/cm) below lower threshold (%.2f). Turning pump ON.\n",
                        data.tdsMScm, EC_LOWER);
          setPump(true);
          pumpWasTurnedOn = true;
        }
      }

      // Turn pump OFF if EC reaches or exceeds upper threshold
      if (data.tdsMScm >= EC_UPPER && pumpActive && pumpWasTurnedOn) {
        if (now - lastPumpOnTime >= MIN_PUMP_ON_MS) {
          Serial.printf("EC (%.2f mS/cm) reached upper threshold (%.2f). Turning pump OFF.\n",
                        data.tdsMScm, EC_UPPER);
          setPump(false);
          pumpWasTurnedOn = false;
        }
      }
    }
  }
  
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 10000) {
    Serial.println("\n=== Sensor Status ===");
    Serial.printf("Air Temp: %.2f C\n", data.dhtTemp);
    Serial.printf("Humidity: %.2f %%\n", data.dhtHumidity);
    Serial.printf("Water Temp: %.2f C\n", data.waterTemp);
    Serial.printf("Flow: %.2f mL/min\n", data.flowRateMlMin);
    Serial.printf("Water Level: %s\n", data.waterLevelHigh ? "HIGH" : "LOW");
    Serial.printf("TDS: %.2f PPM (%.2f mS/cm) [raw ADC: %d]\n", data.tdsPPM, data.tdsMScm, lastTdsRawADC);
    Serial.printf("Pump: %s (Auto: %s, Manual Override: %s)\n", 
                  pumpActive ? "ON" : "OFF", 
                  autoPumpEnabled ? "enabled" : "disabled",
                  pumpManualOverride ? "active" : "inactive");
    lastPrint = millis();
  }
  
  // Heap trend. A steady decline here means something is leaking — this is how
  // a per-request allocation regression would show up over time.
  if (millis() - lastHeapLogMs >= 60000) {
    lastHeapLogMs = millis();
    Serial.printf("[HEAP] free=%u bytes, largest block=%u, uptime=%lus\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), millis() / 1000);
  }

  // Update LCD display every 5 seconds
  updateLCDDisplay();
  
  delay(50);
}
