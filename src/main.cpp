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
#include <FirebaseESP32.h>

// ==================== Firebase Configuration ====================
#define FIREBASE_HOST "https://hydrophonic-bucket-system-default-rtdb.asia-southeast1.firebasedatabase.app/"
#define FIREBASE_AUTH ""  // Add Database Secret if Auth is required

FirebaseData fbdo;
FirebaseAuth fbAuth;
FirebaseConfig fbConfig;
unsigned long lastFirebasePushMs = 0;
unsigned long lastFirebaseHistoryMs = 0;
unsigned long lastFirebaseControlMs = 0;
String lastPumpCommand      = "";
String lastAutoPumpCommand  = "";
String lastWaterPumpCommand = "";

// ==================== WiFi Configuration ====================
const char* ssid = "Infinix NOTE 40";
const char* password = "Achi@234";

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
#define DEFAULT_GSM_PHONE "+94740879724"
#define DEFAULT_GSM_MESSAGE "Water level is LOW"
#define I2C_SDA 21           // I2C Data line for LCD
#define I2C_SCL 22           // I2C Clock line for LCD

// ==================== EC Auto Pump Control ====================
const float EC_LOWER = 2.2f;         // Pump turns ON when EC drops below this
const float EC_UPPER = 2.8f;         // Pump turns OFF when EC reaches this
const unsigned long MIN_PUMP_ON_MS = 10000;  // Minimum pump run time (10s)
const unsigned long MIN_PUMP_OFF_MS = 10000; // Minimum pump off time (10s)
const unsigned long MANUAL_OVERRIDE_TIMEOUT_MS = 300000; // Manual override timeout (5 min)

const bool RELAY_ACTIVE_LOW = true;   // Most relay boards are active LOW
const float FLOW_PULSES_PER_LITER = 450.0f; // Approx. YF-S201: 7.5 pulses/sec = 1 L/min => 450 pulses/L
const float ADC_REF_VOLTAGE = 3.3f;
const int ADC_MAX = 4095;

// ==================== Sensor Objects ====================
DHT dht(DHT_PIN, DHT22);
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature tempSensor(&oneWire);
AsyncWebServer server(80);
LiquidCrystal_I2C lcd(0x27, 16, 2);  // I2C address 0x27, 16 columns, 2 rows
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
bool gsmEnabled = false;
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
    Serial.println("[WATER PUMP ON]");
  } else {
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
  gsmEnabled = gsmPrefs.getBool("enabled", false);
  gsmPhoneNumber = gsmPrefs.getString("phone", DEFAULT_GSM_PHONE);
  gsmAlertMessage = gsmPrefs.getString("msg", DEFAULT_GSM_MESSAGE);
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
float readTdsPPM() {
  const int samples = 10;
  long total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogRead(TDS_PIN);
    delay(5);
  }
  float rawADC = total / (float)samples;
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

SensorData readAllSensors() {
  SensorData data{};
  data.timestamp = millis();

  float airTemp = dht.readTemperature();
  float humidity = dht.readHumidity();
  data.dhtTemp = isnan(airTemp) ? -1 : airTemp;
  data.dhtHumidity = isnan(humidity) ? -1 : humidity;

  tempSensor.requestTemperatures();
  float waterTemp = tempSensor.getTempCByIndex(0);
  data.waterTemp = (waterTemp == 85.0f || waterTemp == -127.0f) ? -1 : waterTemp;

  // Float switch with INPUT_PULLUP: HIGH means water level is high, LOW means low
  bool rawFloat = digitalRead(FLOAT_SWITCH_PIN);
  data.waterLevelHigh = (rawFloat == HIGH);
  waterLevelHigh = data.waterLevelHigh;

  data.flowRateMlMin = readFlowMlMin();
  data.tdsPPM = readTdsPPM();
  data.tdsMScm = data.tdsPPM / 640.0f;

  return data;
}

String buildSensorJson() {
  SensorData data = readAllSensors();
  JsonDocument doc;
  doc["dhtTemperature"] = data.dhtTemp;
  doc["dhtHumidity"] = data.dhtHumidity;
  doc["waterTemperature"] = data.waterTemp;
  doc["flowRate"] = data.flowRateMlMin;
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
  lcd.setCursor(0, 0);
  lcd.print("Hydroponic");
  lcd.setCursor(0, 1);
  lcd.print("Dashboard");
  lcdAvailable = true;
  Serial.println("LCD initialized successfully");
}

void updateLCDDisplay() {
  if (!lcdAvailable) return;
  
  unsigned long now = millis();
  if (now - lastLcdUpdate < LCD_UPDATE_INTERVAL) return;
  lastLcdUpdate = now;
  
  SensorData data = readAllSensors();
  
  // Row 1: Pump status + Water temperature + Auto status
  char row1[17] = {0};
  snprintf(row1, 17, "P:%s A:%s", pumpActive ? "ON " : "OFF", autoPumpEnabled ? "ON" : "OF");
  
  // Row 2: EC level (mS/cm)
  char row2[17] = {0};
  snprintf(row2, 17, "EC:%.2fmS/cm", data.tdsMScm);
  
  lcd.setCursor(0, 0);
  lcd.print(row1);
  lcd.setCursor(0, 1);
  lcd.print(row2);
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

    // Check first character (case already converted to lowercase)
    if (action.length() > 0) {
      if (action[0] == 'e') {  // 'enable' starts with 'e'
        setAutoPump(true);
        pumpManualOverride = false;
      } else if (action[0] == 'd') {  // 'disable' starts with 'd'
        setAutoPump(false);
        pumpManualOverride = false;
      } else {
        request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid action\"}");
        return;
      }
    } else {
      request->send(400, "application/json", "{\"status\":\"error\",\"message\":\"Invalid action\"}");
      return;
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
  } else {
    Serial.println("\nWiFi failed. Starting Access Point mode.");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("HydroponicDashboard", "hydroponic123");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }
}

void setupFirebase() {
  fbConfig.database_url = FIREBASE_HOST;
  fbConfig.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&fbConfig, &fbAuth);
  Firebase.reconnectWiFi(true);
  Serial.println("[Firebase] Realtime Database configured");
}

void updateFirebase(const SensorData &data) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  unsigned long now = millis();

  // Push live sensor state to Firebase every 5 seconds
  if (now - lastFirebasePushMs >= 5000) {
    lastFirebasePushMs = now;

    FirebaseJson json;
    json.set("dhtTemperature", data.dhtTemp);
    json.set("dhtHumidity", data.dhtHumidity);
    json.set("waterTemperature", data.waterTemp);
    json.set("flowRate", data.flowRateMlMin / 1000.0f);
    json.set("waterLevel", data.waterLevelHigh ? "HIGH" : "LOW");
    json.set("tdsPPM", data.tdsPPM);
    json.set("tdsMScm", data.tdsMScm);
    json.set("pumpActive", pumpActive);
    json.set("waterPumpActive", waterPumpActive);
    json.set("autoPumpEnabled", autoPumpEnabled);
    json.set("pumpManualOverride", pumpManualOverride);
    json.set("timestamp", millis());

    if (!Firebase.setJSON(fbdo, "/sensors/live", json)) {
      Serial.printf("[Firebase ERROR] %s\n", fbdo.errorReason().c_str());
    }
  }

  // Push historical log entry every 60 seconds
  if (now - lastFirebaseHistoryMs >= 60000) {
    lastFirebaseHistoryMs = now;

    FirebaseJson json;
    json.set("dhtTemperature", data.dhtTemp);
    json.set("dhtHumidity", data.dhtHumidity);
    json.set("waterTemperature", data.waterTemp);
    json.set("flowRate", data.flowRateMlMin / 1000.0f);
    json.set("waterLevel", data.waterLevelHigh ? "HIGH" : "LOW");
    json.set("tdsMScm", data.tdsMScm);
    json.set("timestamp", millis());

    Firebase.pushJSON(fbdo, "/sensors/history", json);
  }
}

// ==================== Remote Control via Firebase (Netlify → ESP32) ====================
void pollFirebaseControls() {
  if (WiFi.status() != WL_CONNECTED) return;
  unsigned long now = millis();
  if (now - lastFirebaseControlMs < 2000) return;  // Poll every 2 seconds
  lastFirebaseControlMs = now;

  // --- Air Pump Control ---
  FirebaseData ctrlFbdo;
  if (Firebase.getString(ctrlFbdo, "/controls/pump")) {
    String cmd = ctrlFbdo.stringData();
    cmd.toLowerCase(); cmd.trim();
    if (cmd != lastPumpCommand && cmd.length() > 0) {
      lastPumpCommand = cmd;
      if (cmd == "on") {
        setPump(true);
        pumpManualOverride = true;
        pumpManualOverrideTimeout = millis() + MANUAL_OVERRIDE_TIMEOUT_MS;
        Serial.println("[Firebase] Remote command: Air Pump ON");
      } else if (cmd == "off") {
        setPump(false);
        pumpManualOverride = true;
        pumpManualOverrideTimeout = millis() + MANUAL_OVERRIDE_TIMEOUT_MS;
        Serial.println("[Firebase] Remote command: Air Pump OFF");
      }
    }
  }

  // --- Auto Pump Control ---
  if (Firebase.getString(ctrlFbdo, "/controls/autoPump")) {
    String cmd = ctrlFbdo.stringData();
    cmd.toLowerCase(); cmd.trim();
    if (cmd != lastAutoPumpCommand && cmd.length() > 0) {
      lastAutoPumpCommand = cmd;
      if (cmd == "enable") {
        setAutoPump(true);
        pumpManualOverride = false;
        Serial.println("[Firebase] Remote command: Auto Pump ENABLED");
      } else if (cmd == "disable") {
        setAutoPump(false);
        pumpManualOverride = false;
        Serial.println("[Firebase] Remote command: Auto Pump DISABLED");
      }
    }
  }

  // --- Water Pump Control ---
  if (Firebase.getString(ctrlFbdo, "/controls/waterPump")) {
    String cmd = ctrlFbdo.stringData();
    cmd.toLowerCase(); cmd.trim();
    if (cmd != lastWaterPumpCommand && cmd.length() > 0) {
      lastWaterPumpCommand = cmd;
      if (cmd == "on") {
        setWaterPump(true);
        Serial.println("[Firebase] Remote command: Water Pump ON");
      } else if (cmd == "off") {
        setWaterPump(false);
        Serial.println("[Firebase] Remote command: Water Pump OFF");
      }
    }
  }
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
  attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN), flowISR, RISING);
  lastFlowCalcMs = millis();

  setupLCD();
  setupSPIFFS();
  setupWiFi();
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
  
  SensorData data = readAllSensors();
  checkFloatSwitchAlert(data.waterLevelHigh);
  updateFirebase(data);
  pollFirebaseControls();  // Handle remote commands from Netlify dashboard

  // Auto pump control logic (only if auto mode enabled and no manual override)
  if (autoPumpEnabled && !pumpManualOverride) {
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
  
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint >= 10000) {
    Serial.println("\n=== Sensor Status ===");
    Serial.printf("Air Temp: %.2f C\n", data.dhtTemp);
    Serial.printf("Humidity: %.2f %%\n", data.dhtHumidity);
    Serial.printf("Water Temp: %.2f C\n", data.waterTemp);
    Serial.printf("Flow: %.2f mL/min\n", data.flowRateMlMin);
    Serial.printf("Water Level: %s\n", data.waterLevelHigh ? "HIGH" : "LOW");
    Serial.printf("TDS: %.2f PPM (%.2f mS/cm)\n", data.tdsPPM, data.tdsMScm);
    Serial.printf("Pump: %s (Auto: %s, Manual Override: %s)\n", 
                  pumpActive ? "ON" : "OFF", 
                  autoPumpEnabled ? "enabled" : "disabled",
                  pumpManualOverride ? "active" : "inactive");
    lastPrint = millis();
  }
  
  // Update LCD display every 5 seconds
  updateLCDDisplay();
  
  delay(50);
}
