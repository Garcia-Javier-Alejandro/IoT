#include <Arduino.h>

#include <WiFi.h>              // ESP32 WiFi
#include <WiFiClientSecure.h>  // TLS Client (HTTPS/MQTTS)
#include <WiFiManager.h>       // WiFiManager for captive portal provisioning (fallback)
#include <PubSubClient.h>      // MQTT client (uses a Client underneath)
#include <time.h>              // For NTP (system time)
#include <OneWire.h>           // OneWire protocol for DS18B20
#include <DallasTemperature.h> // DS18B20 temperature sensor library
#include <Preferences.h>       // NVS storage for WiFi credentials

// =================== Project Includes ====================
#include "config.h"    // host/ports/topics/device_id (NO secrets)
#include "secrets.h"   // wifi and mqtt user/pass (SECRET)
#include "ca_cert.h"   // Root CA certificate (public)
#include "ble_provisioning.h"  // BLE provisioning for WiFi credentials

// ==================== Timing Constants ====================
#define VALVE_SWITCH_DELAY      500       // Delay for valve switching (ms)
#define WIFI_CONNECT_TIMEOUT    15000     // Timeout for WiFi connection (ms)
#define WIFI_RECONNECT_INTERVAL 10000     // Check WiFi status every 10 seconds
#define WIFI_RETRY_ATTEMPTS     3         // Number of connection retry attempts
#define WIFI_RETRY_DELAY        5000      // Delay between retry attempts (ms)
#define NTP_SYNC_TIMEOUT        15000     // Timeout for NTP synchronization (ms)
#define WIFI_STATE_INTERVAL     30000     // Interval to publish WiFi state (ms)
#define TIMER_PUBLISH_INTERVAL  10000     // Interval to publish timer state (ms)
#define TEMP_PUBLISH_INTERVAL   60000     // Interval to publish temperature (ms) - 60 seconds
#define BLE_CHECK_INTERVAL      1000      // Check for BLE credentials every 1 second
#define MIN_VALID_EPOCH         1700000000L // Minimum valid epoch for NTP (Nov 2023)

// ==================== Hardware State ====================
static bool pumpState = false;     // Logical pump state (ON/OFF)
static int valveMode = 1;          // Valve mode: 1 or 2
static float currentTemperature = 0.0; // Current temperature in °C
static bool wifiProvisioned = false;   // Flag to track if provisioning completed

// ==================== Timer State ====================
static bool timerActive = false;   // Timer is running
static int timerMode = 1;          // Timer mode (1=Cascada, 2=Eyectores)
static uint32_t timerDuration = 0; // Total timer duration in seconds
static uint32_t timerRemaining = 0; // Remaining time in seconds
static uint32_t timerLastUpdate = 0; // Last millis() for countdown

// ==================== Temperature Sensor ====================
// Setup OneWire on GPIO 21
OneWire oneWire(TEMP_SENSOR_PIN);
DallasTemperature tempSensor(&oneWire);

// ==================== MQTT/TLS ====================
// TLS Client (used to connect to a server with certificate)
WiFiClientSecure tlsClient;

// MQTT Client that travels over the tlsClient
PubSubClient mqtt(tlsClient);

// ==================== Forward Declarations ====================
void clearWiFiCredentials();

// ==================== Helper Functions ====================

/**
 * Converts MQTT payload (bytes) to String
 * @param payload Byte array received from MQTT broker
 * @param length Payload length in bytes
 * @return String with payload content (trimmed)
 */
String payloadToString(const byte* payload, unsigned int length) {
  String s;
  s.reserve(length);
  for (unsigned int i = 0; i < length; i++) s += (char)payload[i];
  s.trim();
  return s;
}

// ==================== Temperature Sensor ====================

/**
 * Reads temperature from DS18B20 sensor
 * @return Temperature in Celsius degrees, or NAN if error
 */
float readTemperature() {
  tempSensor.requestTemperatures();
  float temp = tempSensor.getTempCByIndex(0);
  
  Serial.print("[SENSOR] Temperature: ");
  if (temp == DEVICE_DISCONNECTED_C) {
    Serial.println("ERROR - sensor desconectado");
    return NAN;
  }
  Serial.print(temp);
  Serial.println(" °C");
  
  return temp;
}

// ==================== MQTT State Publishing ====================

/**
 * Publishes current pump state to MQTT topic
 * Uses retain=true so last value is stored in the broker
 */
void publishPumpState() {
  const char* msg = pumpState ? "ON" : "OFF";
  bool ok = mqtt.publish(TOPIC_PUMP_STATE, msg, true /*retain*/);
  
  Serial.print("[MQTT] publish ");
  Serial.print(TOPIC_PUMP_STATE);
  Serial.print(" = ");
  Serial.print(msg);
  Serial.println(ok ? " OK" : " FAIL");
}

/**
 * Publishes current valve state to MQTT topic
 * Sends "1" or "2" depending on active mode
 */
void publishValveState() {
  char msg[2];
  msg[0] = '0' + valveMode;  // Convert 1 or 2 to "1" or "2"
  msg[1] = '\0';
  
  bool ok = mqtt.publish(TOPIC_VALVE_STATE, msg, true /*retain*/);
  
  Serial.print("[MQTT] publish ");
  Serial.print(TOPIC_VALVE_STATE);
  Serial.print(" = ");
  Serial.print(msg);
  Serial.println(ok ? " OK" : " FAIL");
}

/**
 * Publishes complete WiFi state in JSON format
 * Includes: status, SSID, IP, RSSI (signal), quality
 * Quality is determined based on RSSI:
 * - excellent: >= -50 dBm
 * - good: >= -60 dBm
 * - fair: >= -70 dBm
 * - weak: < -70 dBm
 */
void publishWiFiState() {
  if (WiFi.status() != WL_CONNECTED) {
    mqtt.publish(TOPIC_WIFI_STATE, "{\"status\":\"disconnected\"}", true);
    return;
  }
  
  int rssi = WiFi.RSSI();
  String quality;
  
  // Determinar calidad de señal basado en RSSI
  if (rssi >= -50) quality = "excellent";
  else if (rssi >= -60) quality = "good";
  else if (rssi >= -70) quality = "fair";
  else quality = "weak";
  
  // Build JSON
  String json = "{";
  json += "\"status\":\"connected\",";
  json += "\"ssid\":\"" + WiFi.SSID() + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"quality\":\"" + quality + "\"";
  json += "}";
  
  bool ok = mqtt.publish(TOPIC_WIFI_STATE, json.c_str(), true /*retain*/);
  
  Serial.print("[MQTT] publish ");
  Serial.print(TOPIC_WIFI_STATE);
  Serial.print(" = ");
  Serial.print(json);
  Serial.println(ok ? " OK" : " FAIL");
}

/**
 * Publishes timer state in JSON format
 * Includes: active (bool), remaining (seconds), mode (1 or 2), duration (total seconds)
 */
void publishTimerState() {
  String json = "{";
  json += "\"active\":" + String(timerActive ? "true" : "false") + ",";
  json += "\"remaining\":" + String(timerRemaining) + ",";
  json += "\"mode\":" + String(timerMode) + ",";
  json += "\"duration\":" + String(timerDuration);
  json += "}";
  
  bool ok = mqtt.publish(TOPIC_TIMER_STATE, json.c_str(), true /*retain*/);
  
  Serial.print("[MQTT] publish ");
  Serial.print(TOPIC_TIMER_STATE);
  Serial.print(" = ");
  Serial.print(json);
  Serial.println(ok ? " OK" : " FAIL");
}

/**
 * Publishes current temperature to MQTT topic
 * Format: decimal number with 1 decimal place (e.g., "25.3")
 */
void publishTemperature() {
  if (isnan(currentTemperature)) {
    Serial.println("[MQTT] Skip temperature publish - invalid reading");
    // Publish diagnostic to error topic for visibility
    String err = "{\"error\":\"sensor_disconnected\"}";
    mqtt.publish(TOPIC_TEMP_ERROR, err.c_str(), true /*retain*/);
    Serial.print("[MQTT] publish ");
    Serial.print(TOPIC_TEMP_ERROR);
    Serial.print(" = ");
    Serial.println(err);
    return;
  }
  
  char tempStr[8];
  dtostrf(currentTemperature, 4, 1, tempStr); // Format: "XX.X"
  
  bool ok = mqtt.publish(TOPIC_TEMP_STATE, tempStr, true);
  
  Serial.print("[MQTT] publish ");
  Serial.print(TOPIC_TEMP_STATE);
  Serial.print(" = ");
  Serial.print(tempStr);
  Serial.println(ok ? " OK" : " FAIL");
}

// ==================== Relay Control ====================

/**
 * Controls pump relay with continuous state
 * @param targetState Desired state: true=ON, false=OFF
 */
void setPumpRelay(bool targetState) {
  Serial.print("[RELAY] Pump relay: ");
  Serial.println(targetState ? "ON" : "OFF");
  
  digitalWrite(PUMP_RELAY_PIN, targetState ? HIGH : LOW);
  pumpState = targetState;
}

/**
 * Controls valve relay (NC+NO in parallel)
 * LOW = Mode 1 (Cascada), HIGH = Mode 2 (Eyectores)
 * @param targetMode Desired mode: 1 (Cascada) or 2 (Eyectores)
 */
void setValveRelay(int targetMode) {
  if (targetMode != 1 && targetMode != 2) {
    Serial.println("[RELAY] ERROR: Invalid valve mode. Use 1 or 2");
    return;
  }
  
  Serial.print("[RELAY] Valve relay: Mode ");
  Serial.println(targetMode);
  
  // Mode 1 (Cascada) = LOW, Mode 2 (Eyectores) = HIGH
  digitalWrite(VALVE_RELAY_PIN, (targetMode == 2) ? HIGH : LOW);
  valveMode = targetMode;
}

// ==================== Control Logic ====================

/**
 * Controls pump: sets relay state and publishes
 * @param targetState Desired state: true=ON, false=OFF
 */
void setPumpState(bool targetState) {
  Serial.print("[CONTROL] Pump target state: ");
  Serial.println(targetState ? "ON" : "OFF");
  
  setPumpRelay(targetState);
  publishPumpState();
}

/**
 * Controls valves: switches to specified mode
 * Validates mode is valid (1 or 2) and avoids unnecessary pulses
 * if already in desired mode
 * @param targetMode Desired mode: 1 (Cascada) or 2 (Eyectores)
 */
void setValveMode(int targetMode) {
  if (targetMode != 1 && targetMode != 2) {
    Serial.println("[CONTROL] ERROR: Invalid valve mode. Use 1 or 2");
    return;
  }
  
  Serial.print("[CONTROL] Valve target mode: ");
  Serial.println(targetMode);
  
  if (valveMode == targetMode) {
    Serial.println("[CONTROL] Valve already in target mode");
    publishValveState();
    return;
  }
  
  setValveRelay(targetMode);
  publishValveState();
}

// ==================== Timer Control ====================

/**
 * Starts timer with specified mode and duration
 * Sequence:
 * 1. Validates parameters (mode 1 or 2, duration > 0)
 * 2. Configures timer variables
 * 3. Sets valve mode
 * 4. Turns on pump
 * 5. Publishes initial state
 * @param mode Valve mode: 1 (Cascada) or 2 (Eyectores)
 * @param durationSeconds Duration in seconds
 */
void startTimer(int mode, uint32_t durationSeconds) {
  if (mode != 1 && mode != 2) {
    Serial.println("[TIMER] ERROR: Invalid mode. Use 1 or 2");
    return;
  }
  
  if (durationSeconds == 0) {
    Serial.println("[TIMER] ERROR: Duration must be > 0");
    return;
  }
  
  Serial.print("[TIMER] Starting timer: mode=");
  Serial.print(mode);
  Serial.print(", duration=");
  Serial.print(durationSeconds);
  Serial.println("s");
  
  // Configure timer
  timerActive = true;
  timerMode = mode;
  timerDuration = durationSeconds;
  timerRemaining = durationSeconds;
  timerLastUpdate = millis();
  
  // Set valve mode
  setValveMode(mode);
  delay(VALVE_SWITCH_DELAY); // Wait for valves to switch completely
  
  // Turn on pump
  setPumpState(true);
  
  // Publish initial timer state
  publishTimerState();
}

/**
 * Stops timer
 * Turns off pump and publishes new state (inactive)
 */
void stopTimer() {
  if (!timerActive) return;
  
  Serial.println("[TIMER] Stopping timer");
  
  timerActive = false;
  timerRemaining = 0;
  
  // Turn off pump
  setPumpState(false);
  
  // Publish timer state
  publishTimerState();
}

/**
 * Updates timer countdown (call in loop)
 * Decrements remaining time every second and publishes state periodically
 * When timer expires (remaining=0), stops automatically
 */
void updateTimer() {
  if (!timerActive) return;
  
  uint32_t now = millis();
  uint32_t elapsed = (now - timerLastUpdate) / 1000; // Elapsed seconds
  
  if (elapsed >= 1) {
    timerLastUpdate = now;
    
    if (timerRemaining > 0) {
      timerRemaining--;
      
      // Publish state every 10 seconds or when little time remains
      static uint32_t lastPublish = 0;
      if (timerRemaining % 10 == 0 || timerRemaining <= 10 || (now - lastPublish) > TIMER_PUBLISH_INTERVAL) {
        lastPublish = now;
        publishTimerState();
      }
      
      // Display remaining time on Serial
      if (timerRemaining % 60 == 0 || timerRemaining <= 60) {
        Serial.print("[TIMER] Remaining: ");
        Serial.print(timerRemaining / 60);
        Serial.print("m ");
        Serial.print(timerRemaining % 60);
        Serial.println("s");
      }
    } else {
      // Timer finished
      Serial.println("[TIMER] Time expired!");
      stopTimer();
    }
  }
}

// ==================== MQTT Message Handler ====================

/**
 * Callback invoked when MQTT message arrives
 * Handles 3 types of commands:
 * 1. Pump (TOPIC_PUMP_SET): ON/OFF/TOGGLE
 * 2. Valves (TOPIC_VALVE_SET): 1/2/TOGGLE
 * 3. Timer (TOPIC_TIMER_SET): JSON with {mode, duration}
 * @param topic Topic of received message
 * @param payload Message content (bytes)
 * @param length Payload length
 */
void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String msg = payloadToString(payload, length);
  msg.toUpperCase();

  Serial.print("[MQTT] RX ");
  Serial.print(t);
  Serial.print(" : ");
  Serial.println(msg);

  // ===== Pump Control =====
  if (t == TOPIC_PUMP_SET) {
    if (msg == "ON" || msg == "1") {
      setPumpState(true);
    } else if (msg == "OFF" || msg == "0") {
      setPumpState(false);
    } else if (msg == "TOGGLE") {
      // Toggle: invert current state
      setPumpState(!pumpState);
    } else {
      Serial.println("[MQTT] Unknown pump command. Use: ON/OFF/TOGGLE");
    }
    return;
  }

  // ===== Valve Control =====
  if (t == TOPIC_VALVE_SET) {
    if (msg == "1") {
      setValveMode(1);
    } else if (msg == "2") {
      setValveMode(2);
    } else if (msg == "TOGGLE") {
      // Toggle: alternate between mode 1 and 2
      setValveMode(valveMode == 1 ? 2 : 1);
    } else {
      Serial.println("[MQTT] Unknown valve command. Use: 1/2/TOGGLE");
    }
    return;
  }

  // ===== Timer Control =====
  if (t == TOPIC_TIMER_SET) {
    // Parse simple JSON: {"mode": 1, "duration": 3600}
    // Note: Uses manual parsing instead of ArduinoJson to save memory
    int modeIdx = msg.indexOf("\"mode\":");
    int durationIdx = msg.indexOf("\"duration\":");
    
    if (modeIdx == -1 || durationIdx == -1) {
      Serial.println("[MQTT] ERROR: Timer command must be JSON with mode and duration");
      return;
    }
    
    // Extract values (simple parsing)
    int modeStart = msg.indexOf(":", modeIdx) + 1;
    int modeEnd = msg.indexOf(",", modeStart);
    if (modeEnd == -1) modeEnd = msg.indexOf("}", modeStart);
    String modeStr = msg.substring(modeStart, modeEnd);
    modeStr.trim();
    int mode = modeStr.toInt();
    
    int durationStart = msg.indexOf(":", durationIdx) + 1;
    int durationEnd = msg.indexOf("}", durationStart);
    if (durationEnd == -1) durationEnd = msg.length();
    String durationStr = msg.substring(durationStart, durationEnd);
    durationStr.trim();
    uint32_t duration = durationStr.toInt();
    
    if (duration == 0) {
      // Command to stop timer
      Serial.println("[MQTT] Timer stop command received");
      stopTimer();
    } else {
      // Command to start timer
      Serial.print("[MQTT] Timer start command: mode=");
      Serial.print(mode);
      Serial.print(", duration=");
      Serial.println(duration);
      startTimer(mode, duration);
    }
    return;
  }

  // ===== Temperature Refresh Command =====
  if (t == TOPIC_TEMP_REFRESH) {
    Serial.println("[MQTT] Temperature refresh command received");
    // Force immediate temperature reading
    currentTemperature = readTemperature();
    if (mqtt.connected()) {
      publishTemperature();
    }
    return;
  }

  // ===== WiFi Clear Command =====
  if (t == TOPIC_WIFI_CLEAR) {
    Serial.println("[MQTT] WiFi clear command received from dashboard");
    
    // Publish disconnected state before dropping connection
    mqtt.publish(TOPIC_WIFI_STATE, "{\"status\":\"disconnected\"}", true /*retain*/);
    delay(100); // Let message send
    mqtt.disconnect();
    
    // Disconnect WiFi and erase credentials
    WiFi.disconnect(true /*wifioff*/, true /*erasePersistent*/);
    clearWiFiCredentials();
    
    Serial.println("[WiFi] Credentials erased. Restarting in 2 seconds...");
    delay(2000);
    
    // Restart ESP32 to cleanly enter BLE provisioning mode
    ESP.restart();
    return;
  }
}


// ==================== WiFi Connection (Provisioning) ====================

// NVS storage instance for WiFi credentials
Preferences preferences;

/**
 * Load WiFi credentials from NVS (non-volatile storage)
 * @param ssid Buffer for SSID (min 33 bytes)
 * @param password Buffer for password (min 64 bytes)
 * @return true if credentials exist in NVS, false otherwise
 */
bool loadWiFiCredentials(char* ssid, char* password) {
  preferences.begin("wifi", true); // read-only
  
  String savedSSID = preferences.getString("ssid", "");
  String savedPassword = preferences.getString("password", "");
  
  preferences.end();
  
  if (savedSSID.length() == 0) {
    Serial.println("[NVS] No WiFi credentials stored");
    return false;
  }
  
  strncpy(ssid, savedSSID.c_str(), 32);
  ssid[32] = '\0';
  strncpy(password, savedPassword.c_str(), 63);
  password[63] = '\0';
  
  Serial.print("[NVS] ✓ Loaded WiFi credentials for: ");
  Serial.println(ssid);
  return true;
}

/**
 * Save WiFi credentials to NVS (non-volatile storage)
 * @param ssid WiFi SSID
 * @param password WiFi password
 */
void saveWiFiCredentials(const char* ssid, const char* password) {
  preferences.begin("wifi", false); // read-write
  
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  
  preferences.end();
  
  Serial.print("[NVS] ✓ Saved WiFi credentials for: ");
  Serial.println(ssid);
}

/**
 * Clear WiFi credentials from NVS
 * Useful for testing or factory reset
 */
void clearWiFiCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
  Serial.println("[NVS] WiFi credentials cleared");
}

/**
 * Connect to WiFi using stored credentials with retry logic
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @param retryAttempts Number of connection attempts (default: WIFI_RETRY_ATTEMPTS)
 * @return true if connected successfully, false otherwise
 */
bool connectWiFi(const char* ssid, const char* password, int retryAttempts = WIFI_RETRY_ATTEMPTS) {
  Serial.print("[WiFi] Connecting to: ");
  Serial.println(ssid);
  
  for (int attempt = 1; attempt <= retryAttempts; attempt++) {
    if (attempt > 1) {
      Serial.print("[WiFi] Retry attempt ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.println(retryAttempts);
      delay(WIFI_RETRY_DELAY);
    }
    
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    
    uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < WIFI_CONNECT_TIMEOUT) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("[WiFi] ✓ CONNECTED");
      Serial.print("[WiFi] SSID: ");
      Serial.println(WiFi.SSID());
      Serial.print("[WiFi] IP: ");
      Serial.println(WiFi.localIP());
      Serial.print("[WiFi] RSSI: ");
      Serial.print(WiFi.RSSI());
      Serial.println(" dBm");
      wifiProvisioned = true;
      return true;
    }
    
    if (attempt < retryAttempts) {
      Serial.print("[WiFi] Connection failed, waiting ");
      Serial.print(WIFI_RETRY_DELAY / 1000);
      Serial.println(" seconds before retry...");
    }
  }
  
  Serial.print("[WiFi] ✗ Connection FAILED after ");
  Serial.print(retryAttempts);
  Serial.println(" attempts");
  return false;
}

/**
 * Try fallback WiFi credentials compiled in secrets.h
 * Saves working credentials to NVS so next boot can auto-connect quickly.
 */
bool connectUsingConfiguredFallbacks() {
  const char* ssids[] = {WIFI_SSID, WIFI_SSID_2, WIFI_SSID_3};
  const char* passwords[] = {WIFI_PASS, WIFI_PASS_2, WIFI_PASS_3};

  for (int i = 0; i < 3; i++) {
    if (ssids[i] == nullptr || ssids[i][0] == '\0') {
      continue;
    }

    Serial.print("[WiFi] Trying configured network #");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.println(ssids[i]);

    if (connectWiFi(ssids[i], passwords[i], WIFI_RETRY_ATTEMPTS)) {
      saveWiFiCredentials(ssids[i], passwords[i]);
      Serial.println("[WiFi] ✓ Connected using configured fallback credentials");
      return true;
    }
  }

  Serial.println("[WiFi] No configured fallback network connected");
  return false;
}

/**
 * Callback for when WiFiManager connects successfully
 */
void onWiFiConnect() {
  Serial.println("[WiFi] ✓ CONNECTED via WiFiManager");
  Serial.print("[WiFi] SSID: ");
  Serial.println(WiFi.SSID());
  Serial.print("[WiFi] IP: ");
  Serial.println(WiFi.localIP());
  Serial.print("[WiFi] RSSI: ");
  Serial.print(WiFi.RSSI());
  Serial.println(" dBm");
  wifiProvisioned = true;
}

/**
 * Callback for when WiFiManager enters AP mode (provisioning)
 */
void onWiFiAPStart(WiFiManager* wm) {
  Serial.println("[WiFi] AP mode started - Captive Portal active");
  Serial.print("[WiFi] Connect to: ");
  Serial.println(wm->getConfigPortalSSID());
  Serial.println("[WiFi] Open your browser at: http://192.168.4.1");
}

/**
 * Initialize WiFi with BLE Provisioning (primary) + WiFiManager fallback
 * 
 * Provisioning flow:
 * 1. Try to load WiFi credentials from NVS
 * 2. If credentials exist, connect to WiFi with multiple retry attempts
 * 3. If connection fails after retries, start BLE provisioning (keeps credentials for auto-retry)
 * 4. If no credentials, start BLE provisioning
 * 5. Wait for credentials from Web Bluetooth dashboard
 * 
 * @return true if connected to WiFi, false if provisioning is in progress
 */
bool initWiFiProvisioning() {
  Serial.println("[WiFi] Starting WiFi provisioning...");
  
  // OPTIONAL: Uncomment to clear credentials for testing
  // clearWiFiCredentials();
  // Serial.println("[WiFi] Credentials cleared for testing");
  
  // Step 1: Try to load credentials from NVS
  char ssid[33];
  char password[64];
  
  if (loadWiFiCredentials(ssid, password)) {
    // Step 2: Try to connect with saved credentials (with retries for power failure recovery)
    Serial.println("[WiFi] Found saved credentials, attempting connection with retries...");
    if (connectWiFi(ssid, password, WIFI_RETRY_ATTEMPTS)) {
      return true;  // Success!
    }
    
    // Credentials exist but connection failed after retries
    // DO NOT clear credentials - network may be temporarily down (power failure)
    // Start BLE provisioning so user can update if needed, but keep trying in loop
    Serial.println("[WiFi] Connection failed after retries - network may be down");
    Serial.println("[WiFi] Keeping credentials for auto-retry. Use BLE/MQTT to update if needed.");
  }

  // Step 3: Try compile-time fallback credentials from secrets.h
  Serial.println("[WiFi] Trying configured fallback networks from secrets.h...");
  if (connectUsingConfiguredFallbacks()) {
    return true;
  }
  
  // Step 4: No credentials or connection failed - start BLE provisioning
  Serial.println("[WiFi] Starting BLE provisioning (credentials preserved for retry)...");
  initBLEProvisioning();
  
  // BLE provisioning is non-blocking - credentials will be received in loop()
  return false;
}

/**
 * Fallback: Use WiFiManager captive portal if BLE provisioning fails
 * This provides backwards compatibility and alternative provisioning method
 * @return true if connected to WiFi, false if failed
 */
bool initWiFiManagerFallback() {
  Serial.println("[WiFi] Starting WiFiManager fallback...");
  
  // Create WiFiManager instance
  WiFiManager wm;
  
  // Configure callbacks
  wm.setAPCallback(onWiFiAPStart);
  
  // Configure portal (3 minute timeout, auto-reset on failure)
  wm.setConfigPortalTimeout(180);  // 3 minutes
  
  // Enable specific features for better captive portal
  wm.setWebServerCallback([]() {
    Serial.println("[WiFi] Web server started at 192.168.4.1");
  });
  
  // Auto-connect with saved credentials
  // If no credentials, opens captive portal
  bool connected = wm.autoConnect("ESP32-Pool-Setup", "");
  
  if (!connected) {
    Serial.println("[WiFi] TIMEOUT: No credentials entered in portal");
    // ESP32 will restart automatically after timeout
    return false;
  }
  
  // Save credentials to NVS for next boot
  saveWiFiCredentials(WiFi.SSID().c_str(), WiFi.psk().c_str());
  
  // Callback manual para cuando se conecta
  onWiFiConnect();
  
  return true;
}

// ==================== NTP Time Synchronization ====================

/**
 * Synchronizes ESP32 clock with NTP servers
 * IMPORTANT: TLS validates certificate dates. If ESP32 has incorrect time,
 * handshake may fail. That's why we synchronize NTP BEFORE connecting to MQTT TLS.
 * @return true if synchronized successfully, false if timeout
 */
bool syncTimeNTP() {
  Serial.println("[NTP] Synchronizing time...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  time_t now = time(nullptr);
  const uint32_t start = millis();

  // Wait until time is "reasonable" (after Nov 2023)
  while (now < MIN_VALID_EPOCH && (millis() - start) < NTP_SYNC_TIMEOUT) {
    Serial.print(".");
    delay(500);
    now = time(nullptr);
  }
  Serial.println();

  if (now < MIN_VALID_EPOCH) {
    Serial.println("[NTP] WARN: not synchronized (timeout). TLS may fail.");
    return false;
  }

  Serial.print("[NTP] ✓ OK epoch: ");
  Serial.println((long)now);
  return true;
}

// ==================== MQTT TLS Connection ====================

/**
 * Configures MQTT client with TLS
 * - Sets server and port (defined in config.h)
 * - Registers callback for incoming messages
 * - Loads root CA certificate for TLS validation
 */
void setupMqtt() {
  // Broker MQTT TLS endpoint (8883)
  mqtt.setServer(MQTT_HOST, MQTT_PORT);

  // Callback for incoming messages
  mqtt.setCallback(onMqttMessage);

  // Load root CA so ESP32 can validate broker certificate
  tlsClient.setCACert(LETS_ENCRYPT_ISRG_ROOT_X1);
}

/**
 * Connects to MQTT broker with authentication
 * After connecting:
 * 1. Subscribes to command topics (pump, valve, timer)
 * 2. Publishes initial state of all components (pump, valve, wifi, timer)
 * 3. Reads and publishes initial temperature
 * @return true if connected successfully, false otherwise
 */
bool connectMqtt() {
  Serial.print("[MQTT] Connecting to ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  // ClientID: should be stable and unique.
  // DEVICE_ID comes from config.h
  const char* clientId = DEVICE_ID;

  // Configure Last Will and Testament (LWT)
  // If connection drops unexpectedly, broker publishes this message automatically
  const char* lwt_topic = TOPIC_WIFI_STATE;
  const char* lwt_message = "{\"status\":\"disconnected\"}";
  uint8_t lwt_qos = 0;
  boolean lwt_retain = true;

  // MQTT_USER / MQTT_PASS vienen de secrets.h
  // connect(clientId, user, pass, willTopic, willQoS, willRetain, willMessage)
  bool ok = mqtt.connect(clientId, MQTT_USER, MQTT_PASS, lwt_topic, lwt_qos, lwt_retain, lwt_message);

  if (!ok) {
    Serial.print("[MQTT] ERROR connect rc=");
    Serial.println(mqtt.state()); // PubSubClient error code
    return false;
  }

  Serial.println("[MQTT] ✓ CONNECTED (with Last Will configured)");

  Serial.println("[MQTT] ✓ CONNECTED");

  // Subscribe to command topics
  mqtt.subscribe(TOPIC_PUMP_SET);
  Serial.print("[MQTT] Subscribed: ");
  Serial.println(TOPIC_PUMP_SET);
  
  mqtt.subscribe(TOPIC_VALVE_SET);
  Serial.print("[MQTT] Subscribed: ");
  Serial.println(TOPIC_VALVE_SET);

  mqtt.subscribe(TOPIC_TIMER_SET);
  Serial.print("[MQTT] Subscribed: ");
  Serial.println(TOPIC_TIMER_SET);

  mqtt.subscribe(TOPIC_WIFI_CLEAR);
  Serial.print("[MQTT] Subscribed: ");
  Serial.println(TOPIC_WIFI_CLEAR);

  mqtt.subscribe(TOPIC_TEMP_REFRESH);
  Serial.print("[MQTT] Subscribed: ");
  Serial.println(TOPIC_TEMP_REFRESH);

  // Publish initial state
  publishPumpState();
  publishValveState();
  publishWiFiState();
  publishTimerState();
  
  // Read and publish initial temperature
  currentTemperature = readTemperature();
  publishTemperature();
  
  return true;
}


void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial.println();
  Serial.println("========================================");
  Serial.println("   ESP32 Pool Control System v2.0");
  Serial.println("========================================");

  // Configure output pins (relays)
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(VALVE_RELAY_PIN, OUTPUT);
  
  // Initial state: all relays off
  digitalWrite(PUMP_RELAY_PIN, LOW);
  digitalWrite(VALVE_RELAY_PIN, LOW);

  // ===== DIAGNOSTIC: Check GPIO 21 idle state (OneWire bus should be HIGH when idle) =====
  pinMode(TEMP_SENSOR_PIN, INPUT);
  int gpio21State = digitalRead(TEMP_SENSOR_PIN);
  Serial.print("[DIAGNOSTIC] GPIO 21 idle state: ");
  Serial.println(gpio21State ? "HIGH (3.3V) ✓" : "LOW (0V) ✗ BUS STUCK!");
  // ========================================================================================

  // Initialize DS18B20 temperature sensor
  Serial.println("[SENSOR] Initializing DS18B20...");
  tempSensor.begin();
  int deviceCount = tempSensor.getDeviceCount();
  Serial.print("[SENSOR] DS18B20 devices found: ");
  Serial.println(deviceCount);

  // Initial state
  pumpState = false;
  valveMode = 1;
  currentTemperature = 0.0;

  // 1) Initialize WiFi with provisioning (BLE primary, WiFiManager fallback)
  bool wifiConnected = initWiFiProvisioning();
  
  if (wifiConnected) {
    // WiFi connected immediately (had saved credentials)
    // 2) Synchronize time for TLS
    syncTimeNTP();

    // 3) Configure and connect MQTT
    setupMqtt();
    connectMqtt();
    
    Serial.println("========================================");
    Serial.println("   System ready");
    Serial.println("========================================");
  } else {
    // BLE provisioning started - waiting for credentials
    Serial.println("========================================");
    Serial.println("   Waiting for BLE provisioning...");
    Serial.println("   Open dashboard to provision device");
    Serial.println("========================================");
  }
}

/**
 * Main loop (executed continuously)
 * Responsibilities:
 * 1. Update timer countdown
 * 2. Publish WiFi state periodically
 * 3. Read and publish temperature periodically
 * 4. Detect and recover WiFi connection loss
 * 5. Detect and recover MQTT connection loss
 * 6. Process incoming MQTT messages (mqtt.loop)
 */
void loop() {
  // ===== BLE Provisioning Check =====
  // If BLE is active, check for new credentials from dashboard
  static uint32_t lastBLECheck = 0;
  if (isBLEProvisioningActive()) {
    // Give BLE stack time to process events (writes, notifications, etc.)
    delay(10);
    
    if (millis() - lastBLECheck > BLE_CHECK_INTERVAL) {
      lastBLECheck = millis();
      
      if (hasNewWiFiCredentials()) {
        char ssid[33];
        char password[64];
        
        if (getBLEWiFiSSID(ssid) && getBLEWiFiPassword(password)) {
          Serial.println("[BLE] ✓ Credentials received from dashboard");
          
          // Stop BLE to free resources (~30-50KB RAM, CPU cycles)
          // Dashboard can use MQTT to clear credentials remotely
          stopBLEProvisioning();
          
          // Try to connect with BLE credentials
          if (connectWiFi(ssid, password)) {
            // Save to NVS for future boots
            saveWiFiCredentials(ssid, password);
            clearBLECredentials();
            
            // Complete system initialization
            Serial.println("[System] Completing initialization...");
            syncTimeNTP();
            setupMqtt();
            connectMqtt();
            
            Serial.println("========================================");
            Serial.println("   Sistema listo (via BLE)");
            Serial.println("========================================");
          } else {
            // Connection failed - restart BLE for retry
            Serial.println("[WiFi] BLE credentials failed - restarting BLE for retry...");
            clearBLECredentials();
            initBLEProvisioning();
          }
        }
      }
    }
    
    // If BLE is running, skip normal operations (WiFi reconnection, MQTT, etc.)
    return;
  }
  
  // ===== Normal Operations (only when WiFi connected) =====
  // Check WiFi status periodically, not every loop (prevents spam)
  static uint32_t lastWiFiCheck = 0;
  static int reconnectAttempts = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastWiFiCheck > WIFI_RECONNECT_INTERVAL) {
    lastWiFiCheck = millis();
    reconnectAttempts++;
    
    // WiFi disconnected - try to reconnect
    Serial.print("[WiFi] Connection lost (attempt ");
    Serial.print(reconnectAttempts);
    Serial.println("), attempting recovery...");
    
    char ssid[33];
    char password[64];
    if (loadWiFiCredentials(ssid, password)) {
      // Try single connection attempt (we'll retry on next interval)
      if (connectWiFi(ssid, password, 1)) {
        reconnectAttempts = 0;  // Reset counter on success
        // Reconnect MQTT after WiFi recovery
        if (!mqtt.connected()) {
          Serial.println("[System] WiFi recovered, reconnecting MQTT...");
          connectMqtt();
        }
      }
    } else {
      // No credentials - restart BLE provisioning (only if not already active)
      if (!isBLEProvisioningActive()) {
        Serial.println("[WiFi] No credentials - starting BLE provisioning...");
        initBLEProvisioning();
        reconnectAttempts = 0;
      }
    }
    return;
  }
  
  // Reset reconnect counter when WiFi is connected
  if (WiFi.status() == WL_CONNECTED && reconnectAttempts > 0) {
    reconnectAttempts = 0;
  }
  
  // If WiFi not connected and BLE not active, just wait
  if (WiFi.status() != WL_CONNECTED) {
    delay(100);
    return;
  }
  
  // Update timer if active
  updateTimer();
  
  // Publish WiFi state periodically
  static uint32_t lastWiFiUpdate = 0;
  if (millis() - lastWiFiUpdate > WIFI_STATE_INTERVAL) {
    lastWiFiUpdate = millis();
    if (mqtt.connected()) {
      publishWiFiState();
    }
  }
  
  // Read and publish temperature periodically (every 1 minute)
  static uint32_t lastTempUpdate = 0;
  if (millis() - lastTempUpdate > TEMP_PUBLISH_INTERVAL) {
    lastTempUpdate = millis();
    currentTemperature = readTemperature();
    if (mqtt.connected()) {
      publishTemperature();
    }
  }
  
  // If MQTT drops, reconnect
  if (!mqtt.connected()) {
    Serial.println("[MQTT] Connection lost, reconnecting...");
    connectMqtt();
  }

  // Keep connection alive and process incoming messages
  mqtt.loop();
}
