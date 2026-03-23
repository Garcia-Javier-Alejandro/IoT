# BLE Provisioning & WiFi Connection - Technical Explanation

This document explains how the ESP32 Pool Controller handles WiFi provisioning using Bluetooth Low Energy (BLE) and manages WiFi connections.

## Table of Contents
1. [Overview](#overview)
2. [BLE Provisioning Architecture](#ble-provisioning-architecture)
3. [WiFi Connection Flow](#wifi-connection-flow)
4. [Code Walkthrough](#code-walkthrough)
5. [Integration in Main Loop](#integration-in-main-loop)

---

## Overview

### Why BLE Provisioning?

Traditional WiFi provisioning methods require the user to connect their phone to the ESP32's WiFi access point, which is cumbersome. **BLE provisioning** allows users to send WiFi credentials directly from the web dashboard using the **Web Bluetooth API** without switching networks.

### Provisioning Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                      ESP32 Boot Sequence                         │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                  ┌───────────────────────┐
                  │ Check NVS for         │
                  │ WiFi credentials      │
                  └───────────────────────┘
                              │
                ┌─────────────┴─────────────┐
                │                           │
         ✓ Credentials                 ✗ No credentials
            found                           │
                │                           │
                ▼                           ▼
    ┌───────────────────────┐   ┌───────────────────────┐
    │ Try WiFi connection   │   │ Start BLE advertising │
    │ (with retries)        │   │ "ESP32-Pool-XXXX"     │
    └───────────────────────┘   └───────────────────────┘
                │                          │
        ┌───────┴────────┐                 │
        │                │                 │
   Connected       Failed (3x)             │
        │                │                 │
        │                └─────────────────┘
        │                          │
        ▼                          ▼
┌───────────────┐      ┌───────────────────────────┐
│ Setup MQTT    │      │ Dashboard connects via    │
│ Publish state │      │ Web Bluetooth API         │
│ Normal ops    │      │ Sends SSID + Password     │
└───────────────┘      └───────────────────────────┘
                                   │
                                   ▼
                       ┌───────────────────────┐
                       │ ESP32 connects WiFi   │
                       │ Saves to NVS          │
                       │ Stops BLE (save power)│
                       │ Setup MQTT & continue │
                       └───────────────────────┘
```

---

## BLE Provisioning Architecture

### Header File: `ble_provisioning.h`

The header defines the public API for BLE provisioning:

```cpp
/**
 * Initialize BLE provisioning service
 * Starts BLE advertising with device name "ESP32-Pool-XXXX" (XXXX = last 4 MAC digits)
 */
void initBLEProvisioning();

/**
 * Stop BLE provisioning and free resources
 * Call this after successful WiFi connection to save power
 */
void stopBLEProvisioning();

/**
 * Check if new WiFi credentials were received via BLE
 * @return true if credentials are ready to be used
 */
bool hasNewWiFiCredentials();

/**
 * Get the WiFi SSID received via BLE
 * @param ssid Buffer to store SSID (minimum 33 bytes)
 * @return true if SSID is available, false otherwise
 */
bool getBLEWiFiSSID(char* ssid);

/**
 * Get the WiFi password received via BLE
 * @param password Buffer to store password (minimum 64 bytes)
 * @return true if password is available, false otherwise
 */
bool getBLEWiFiPassword(char* password);

/**
 * Scan available WiFi networks and return JSON array
 * @return JSON string: [{"ssid":"NETWORK1","rssi":-50,"open":false},...]
 */
String scanWiFiNetworks();
```

**Key Design Decisions:**
- Simple boolean flags to check credential availability
- Buffer-based API to avoid dynamic memory allocation
- Stateful design: credentials are stored internally until cleared

---

### Implementation: `ble_provisioning.cpp`

#### 1. BLE Service UUIDs

Each BLE service needs a unique UUID. These are custom UUIDs for our pool controller:

```cpp
// ==================== BLE UUIDs ====================
// Custom UUIDs for Pool Controller WiFi Provisioning Service
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"  // Main service
#define SSID_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"  // SSID characteristic
#define PASSWORD_CHAR_UUID  "cba1d466-344c-4be3-ab3f-189f80dd7518"  // Password characteristic
#define STATUS_CHAR_UUID    "8d8218b6-97bc-4527-a8db-13094ac06b1d"  // Status notifications
#define NETWORKS_CHAR_UUID  "fa87c0d0-afac-11de-8a39-0800200c9a66"  // WiFi scan results
```

**Why multiple characteristics?**
- **SSID** and **Password** are separate for security (password is write-only)
- **Status** provides real-time feedback to the dashboard
- **Networks** allows the dashboard to request WiFi scans

---

#### 2. Global State Variables

```cpp
// ==================== State Variables ====================
static bool bleActive = false;               // Is BLE currently advertising?
static bool newCredentialsReceived = false;  // Have we received complete credentials?
static String receivedSSID = "";             // SSID from dashboard
static String receivedPassword = "";         // Password from dashboard
static bool deviceConnected = false;         // Is a client connected?
```

**Thread Safety Note:** These are `static` variables scoped to this file. The ESP32 runs on a single core for Arduino code, so we don't need mutexes for these simple flags.

---

#### 3. Server Callbacks (Connection Management)

```cpp
/**
 * Server callback - handles client connect/disconnect events
 */
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Client connected");
    
    // Update status characteristic to notify dashboard of connection
    if (pStatusCharacteristic) {
      pStatusCharacteristic->setValue("connected");
      pStatusCharacteristic->notify();  // Send notification to client
    }
  }

  void onDisconnect(NimBLEServer* pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Client disconnected");
    
    // Restart advertising so others can connect
    // This allows re-provisioning if the first attempt fails
    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] Advertising restarted");
  }
};
```

**Key Points:**
- When client connects, we notify them via the status characteristic
- When client disconnects, we **automatically restart advertising** so the dashboard can reconnect
- This makes the provisioning process resilient to disconnections

---

#### 4. Characteristic Callbacks (Data Reception)

This is where the magic happens - receiving WiFi credentials from the dashboard:

```cpp
/**
 * Characteristic callbacks - handles write events for WiFi credentials
 */
class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string uuid = pCharacteristic->getUUID().toString();
    std::string value = pCharacteristic->getValue();
    
    // ========== SSID Received ==========
    if (uuid == SSID_CHAR_UUID) {
      receivedSSID = String(value.c_str());
      Serial.print("[BLE] SSID received: ");
      Serial.println(receivedSSID);
      
      // Notify dashboard that SSID was received successfully
      if (pStatusCharacteristic) {
        pStatusCharacteristic->setValue("ssid_received");
        pStatusCharacteristic->notify();
      }
    } 
    
    // ========== Password Received ==========
    else if (uuid == PASSWORD_CHAR_UUID) {
      receivedPassword = String(value.c_str());
      Serial.print("[BLE] Password received (");
      Serial.print(receivedPassword.length());
      Serial.println(" chars)");  // Don't log password for security
      
      // Notify dashboard
      if (pStatusCharacteristic) {
        pStatusCharacteristic->setValue("password_received");
        pStatusCharacteristic->notify();
      }
      
      // ========== Check if Both Credentials are Complete ==========
      if (receivedSSID.length() > 0 && receivedPassword.length() > 0) {
        newCredentialsReceived = true;  // Flag for main loop
        Serial.println("[BLE] ✓ WiFi credentials complete");
        
        if (pStatusCharacteristic) {
          pStatusCharacteristic->setValue("credentials_ready");
          pStatusCharacteristic->notify();
        }
      }
    }
    
    // ========== WiFi Scan Request ==========
    else if (uuid == NETWORKS_CHAR_UUID) {
      // Dashboard writes to this characteristic to trigger a scan
      Serial.println("[BLE] Networks scan triggered via write");
      String json = scanWiFiNetworks();  // Perform WiFi scan
      
      // Update the characteristic with scan results
      pCharacteristic->setValue((uint8_t*)json.c_str(), json.length());
      
      // Notify client that new data is available
      pCharacteristic->notify();
    }
  }
};
```

**Flow Explanation:**
1. Dashboard writes SSID → ESP32 stores it and notifies "ssid_received"
2. Dashboard writes password → ESP32 stores it and notifies "password_received"
3. **Both received?** → Set `newCredentialsReceived = true` (main loop will check this)
4. Dashboard can request WiFi scan → ESP32 scans and returns JSON array

---

#### 5. BLE Initialization

```cpp
void initBLEProvisioning() {
  Serial.println("[BLE] Initializing BLE provisioning...");
  
  // ========== Generate Unique Device Name ==========
  // Use last 2 bytes of MAC address to make device identifiable
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char deviceName[32];
  // Example: "Controlador Smart Pool-A3F2-v2"
  snprintf(deviceName, sizeof(deviceName), 
           "Controlador Smart Pool-%02X%02X-v2", mac[4], mac[5]);
  
  Serial.print("[BLE] Device name: ");
  Serial.println(deviceName);
  
  // ========== Initialize NimBLE Stack ==========
  NimBLEDevice::init(deviceName);
  
  // ========== Create BLE Server ==========
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());  // Set connection callbacks
  
  // ========== Create BLE Service ==========
  NimBLEService* pService = pServer->createService(SERVICE_UUID);
  
  // ========== Create SSID Characteristic (Read/Write) ==========
  // Dashboard can write SSID and read it back for confirmation
  pSSIDCharacteristic = pService->createCharacteristic(
    SSID_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
  );
  pSSIDCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pSSIDCharacteristic->setValue("");  // Start empty
  
  // ========== Create Password Characteristic (Write Only) ==========
  // Write-only for security - no reading password back
  pPasswordCharacteristic = pService->createCharacteristic(
    PASSWORD_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE  // No READ for security
  );
  pPasswordCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pPasswordCharacteristic->setValue("");
  
  // ========== Create Status Characteristic (Read/Notify) ==========
  // Allows real-time status updates to dashboard
  pStatusCharacteristic = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  pStatusCharacteristic->setValue("waiting");
  
  // ========== Create Networks Characteristic ==========
  // Write to trigger scan, read/notify to get results
  pNetworksCharacteristic = pService->createCharacteristic(
    NETWORKS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  pNetworksCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pNetworksCharacteristic->setValue("[]");  // Empty array initially
  
  // ========== Start the Service ==========
  pService->start();
  
  // ========== Start Advertising ==========
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);  // Advertise our service UUID
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Apple connection parameters
  pAdvertising->setMaxPreferred(0x12);
  
  NimBLEDevice::startAdvertising();
  
  bleActive = true;
  
  Serial.println("[BLE] ✓ Provisioning service started");
  Serial.println("[BLE] Waiting for dashboard connection...");
}
```

**Key Design Choices:**
- **Unique device name** using MAC address - allows multiple devices in same area
- **Password is write-only** - security best practice
- **Status characteristic with notifications** - provides real-time feedback
- **Apple connection parameters** - ensures compatibility with iOS devices

---

#### 6. WiFi Network Scanning

```cpp
String scanWiFiNetworks() {
  Serial.println("[BLE] Scanning WiFi networks...");
  
  // ========== Reset WiFi Driver for BLE Coexistence ==========
  // BLE and WiFi share the same radio on ESP32
  // Need to properly reset state to avoid conflicts
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(200);  // Give WiFi radio time to initialize
  
  // ========== Perform WiFi Scan ==========
  int numNetworks = WiFi.scanNetworks();
  
  if (numNetworks == 0 || numNetworks == -1) {
    Serial.println("[BLE] No networks found or scan failed");
    return "[]";
  }
  
  Serial.print("[BLE] Found ");
  Serial.print(numNetworks);
  Serial.println(" networks");
  
  // ========== Build JSON Array ==========
  // Format: [{"ssid":"NETWORK1","rssi":-50,"open":false},...]
  String json = "[";
  int networkCount = 0;
  
  for (int i = 0; i < numNetworks; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);  // Signal strength
    uint8_t encryption = WiFi.encryptionType(i);
    bool open = (encryption == WIFI_AUTH_OPEN);
    
    // Skip empty SSIDs (hidden networks)
    if (ssid.length() == 0) continue;
    
    // Build network entry
    String entry = "{\"ssid\":\"";
    entry += ssid;
    entry += "\",\"rssi\":";
    entry += String(rssi);
    entry += ",\"open\":";
    entry += (open ? "true" : "false");
    entry += "}";
    
    // ========== BLE MTU Size Limit ==========
    // BLE has a maximum transmission unit (MTU) of ~500 bytes
    // Keep response under 400 bytes for reliability
    int projectedSize = json.length() + entry.length() + 2; // +2 for comma and ]
    if (projectedSize > 400) {
      Serial.println("[BLE] Network list too large, stopping here");
      break;
    }
    
    if (networkCount > 0) json += ",";
    json += entry;
    networkCount++;
  }
  
  json += "]";
  
  // Clean up scan results
  WiFi.scanDelete();
  
  Serial.print("[BLE] JSON size: ");
  Serial.print(json.length());
  Serial.println(" bytes");
  
  return json;
}
```

**Important Considerations:**
- **BLE/WiFi Coexistence:** ESP32 shares one radio for both BLE and WiFi - requires careful mode switching
- **MTU Limit:** BLE has a maximum packet size (~500 bytes) - we limit to 400 for safety
- **RSSI Values:** Signal strength helps users choose the best network
- **Hidden Networks:** Skipped because they have empty SSID

---

## WiFi Connection Flow

### NVS (Non-Volatile Storage) Credentials

The ESP32 stores WiFi credentials persistently using the Preferences library:

```cpp
// ==================== WiFi Connection (Provisioning) ====================

Preferences preferences;  // NVS storage instance

/**
 * Load WiFi credentials from NVS (non-volatile storage)
 * @param ssid Buffer for SSID (min 33 bytes)
 * @param password Buffer for password (min 64 bytes)
 * @return true if credentials exist in NVS, false otherwise
 */
bool loadWiFiCredentials(char* ssid, char* password) {
  preferences.begin("wifi", true);  // Open "wifi" namespace, read-only
  
  String savedSSID = preferences.getString("ssid", "");
  String savedPassword = preferences.getString("password", "");
  
  preferences.end();
  
  // No credentials stored?
  if (savedSSID.length() == 0) {
    Serial.println("[NVS] No WiFi credentials stored");
    return false;
  }
  
  // Copy to buffers
  strncpy(ssid, savedSSID.c_str(), 32);
  ssid[32] = '\0';  // Null-terminate
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
  preferences.begin("wifi", false);  // Open "wifi" namespace, read-write
  
  preferences.putString("ssid", ssid);
  preferences.putString("password", password);
  
  preferences.end();
  
  Serial.print("[NVS] ✓ Saved WiFi credentials for: ");
  Serial.println(ssid);
}

/**
 * Clear WiFi credentials from NVS
 * Useful for factory reset or re-provisioning
 */
void clearWiFiCredentials() {
  preferences.begin("wifi", false);
  preferences.clear();  // Clear all keys in "wifi" namespace
  preferences.end();
  Serial.println("[NVS] WiFi credentials cleared");
}
```

**NVS Explanation:**
- **Namespaces:** NVS organizes data into namespaces (we use "wifi")
- **Persistence:** Data survives reboots, firmware updates, and power loss
- **Flash Wear:** Preferences library manages flash wear-leveling automatically

---

### WiFi Connection with Retry Logic

```cpp
/**
 * Connect to WiFi using stored credentials with retry logic
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @param retryAttempts Number of connection attempts (default: 3)
 * @return true if connected successfully, false otherwise
 */
bool connectWiFi(const char* ssid, const char* password, 
                 int retryAttempts = WIFI_RETRY_ATTEMPTS) {
  Serial.print("[WiFi] Connecting to: ");
  Serial.println(ssid);
  
  // ========== Retry Loop ==========
  for (int attempt = 1; attempt <= retryAttempts; attempt++) {
    if (attempt > 1) {
      Serial.print("[WiFi] Retry attempt ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.println(retryAttempts);
      delay(WIFI_RETRY_DELAY);  // 5 second delay between attempts
    }
    
    // ========== Start WiFi Connection ==========
    WiFi.mode(WIFI_STA);  // Station mode (client)
    WiFi.begin(ssid, password);
    
    // ========== Wait for Connection ==========
    uint32_t startTime = millis();
    while (WiFi.status() != WL_CONNECTED && 
           millis() - startTime < WIFI_CONNECT_TIMEOUT) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    
    // ========== Check Connection Result ==========
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
      return true;  // Success!
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
```

**Retry Strategy:**
- **3 attempts by default** (configurable via `WIFI_RETRY_ATTEMPTS`)
- **15 second timeout** per attempt (`WIFI_CONNECT_TIMEOUT`)
- **5 second delay** between attempts (`WIFI_RETRY_DELAY`)
- **Total max time:** ~60 seconds (3 × (15s timeout + 5s delay))

**Why Retry?**
- Router might be temporarily unavailable (rebooting, power outage)
- Signal interference might block initial attempts
- Gives system resilience without user intervention

---

### WiFi Provisioning Initialization

This is the main entry point that orchestrates the entire provisioning flow:

```cpp
/**
 * Initialize WiFi with BLE provisioning
 * 
 * Provisioning flow:
 * 1. Try to load WiFi credentials from NVS
 * 2. If credentials exist, connect to WiFi with multiple retry attempts
 * 3. If connection fails after retries, start BLE provisioning (keeps credentials)
 * 4. If no credentials, start BLE provisioning
 * 5. Wait for credentials from Web Bluetooth dashboard
 * 
 * @return true if connected to WiFi, false if provisioning is in progress
 */
bool initWiFiProvisioning() {
  Serial.println("[WiFi] Starting WiFi provisioning...");
  
  // OPTIONAL: Uncomment to clear credentials for testing
  // clearWiFiCredentials();
  
  // ========== Step 1: Try to Load Saved Credentials ==========
  char ssid[33];
  char password[64];
  
  if (loadWiFiCredentials(ssid, password)) {
    // ========== Step 2: Try to Connect with Retries ==========
    Serial.println("[WiFi] Found saved credentials, attempting connection...");
    if (connectWiFi(ssid, password, WIFI_RETRY_ATTEMPTS)) {
      return true;  // ✓ Success! WiFi connected
    }
    
    // ========== Connection Failed After Retries ==========
    // IMPORTANT: Do NOT clear credentials
    // Network might be temporarily down (power outage, router reboot)
    // Keep credentials so device can auto-reconnect when network returns
    Serial.println("[WiFi] Connection failed - network may be down");
    Serial.println("[WiFi] Keeping credentials for auto-retry");
  }
  
  // ========== Step 3: Start BLE Provisioning ==========
  // Either no credentials exist, or connection failed
  Serial.println("[WiFi] Starting BLE provisioning...");
  initBLEProvisioning();
  
  // BLE provisioning is non-blocking
  // Credentials will be received in loop() and processed there
  return false;  // WiFi not connected yet
}
```

**Key Design Decisions:**

1. **Don't Clear Credentials on Connection Failure**
   - Router might be temporarily down (power outage)
   - Keeps system resilient to temporary network issues
   - User can manually clear via MQTT or BLE if needed

2. **Non-Blocking BLE Provisioning**
   - `initBLEProvisioning()` just starts advertising
   - Credentials are processed in `loop()` asynchronously
   - Allows system to remain responsive

3. **Graceful Degradation**
   - If saved credentials work → immediate connection
   - If saved credentials fail → BLE provisioning
   - If no credentials → BLE provisioning

---

## Integration in Main Loop

### Setup Function

```cpp
void setup() {
  Serial.begin(115200);
  delay(500);
  
  Serial.println("========================================");
  Serial.println("   ESP32 Pool Control System v2.0");
  Serial.println("========================================");

  // ========== Configure Hardware Pins ==========
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(VALVE_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);   // Relays off initially
  digitalWrite(VALVE_RELAY_PIN, LOW);

  // ========== Initialize Temperature Sensor ==========
  tempSensor.begin();
  int deviceCount = tempSensor.getDeviceCount();
  Serial.print("[SENSOR] DS18B20 devices found: ");
  Serial.println(deviceCount);

  // ========== Initialize WiFi with Provisioning ==========
  bool wifiConnected = initWiFiProvisioning();
  
  if (wifiConnected) {
    // ✓ WiFi connected immediately (had saved credentials)
    syncTimeNTP();     // Sync time for TLS certificates
    setupMqtt();       // Configure MQTT broker settings
    connectMqtt();     // Connect to MQTT broker
    
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
```

**Setup Flow:**
1. Initialize hardware (relays, sensors)
2. Try WiFi provisioning
3. If WiFi connected → full system initialization
4. If WiFi not connected → wait for BLE provisioning in loop()

---

### Main Loop - BLE Credential Processing

```cpp
void loop() {
  // ========== BLE Provisioning Check ==========
  // If BLE is active, check for new credentials from dashboard
  static uint32_t lastBLECheck = 0;
  
  if (isBLEProvisioningActive()) {
    // Give BLE stack time to process events
    delay(10);
    
    // Check for credentials every 1 second (not every loop iteration)
    if (millis() - lastBLECheck > BLE_CHECK_INTERVAL) {
      lastBLECheck = millis();
      
      // ========== Check if Credentials Received ==========
      if (hasNewWiFiCredentials()) {
        char ssid[33];
        char password[64];
        
        if (getBLEWiFiSSID(ssid) && getBLEWiFiPassword(password)) {
          Serial.println("[BLE] ✓ Credentials received from dashboard");
          
          // ========== Stop BLE to Free Resources ==========
          // BLE uses ~30-50KB RAM and CPU cycles
          // Stop it once we have credentials
          stopBLEProvisioning();
          
          // ========== Try to Connect with BLE Credentials ==========
          if (connectWiFi(ssid, password)) {
            // ✓ Connection successful
            
            // Save to NVS for future boots
            saveWiFiCredentials(ssid, password);
            clearBLECredentials();  // Clear BLE state
            
            // ========== Complete System Initialization ==========
            Serial.println("[System] Completing initialization...");
            syncTimeNTP();
            setupMqtt();
            connectMqtt();
            
            Serial.println("========================================");
            Serial.println("   Sistema listo (via BLE)");
            Serial.println("========================================");
          } else {
            // ✗ Connection failed - restart BLE for retry
            Serial.println("[WiFi] BLE credentials failed - restarting BLE...");
            clearBLECredentials();
            initBLEProvisioning();  // Allow user to try again
          }
        }
      }
    }
    
    // If BLE is running, skip normal operations
    // (no WiFi connection yet, so MQTT/sensors don't make sense)
    return;
  }
  
  // ========== Normal Operations (WiFi Connected) ==========
  // ... rest of loop code (MQTT, sensors, timers, etc.) ...
}
```

**BLE Loop Logic:**

1. **Check every 1 second** (not every iteration) - saves CPU
2. **Credentials received?**
   - Stop BLE to free resources (~30-50KB RAM)
   - Try WiFi connection
   - If success → save to NVS, complete initialization
   - If failure → restart BLE, allow retry
3. **While BLE active** → skip normal operations (early return)

**Why Skip Normal Operations?**
- No WiFi → no MQTT connection
- No point checking sensors if we can't publish data
- Keeps code simple and resource usage low

---

### Main Loop - WiFi Reconnection

```cpp
void loop() {
  // ... BLE provisioning code above ...
  
  // ========== WiFi Status Check (Periodic) ==========
  static uint32_t lastWiFiCheck = 0;
  static int reconnectAttempts = 0;
  
  // Check WiFi status every 10 seconds (not every loop)
  if (WiFi.status() != WL_CONNECTED && 
      millis() - lastWiFiCheck > WIFI_RECONNECT_INTERVAL) {
    lastWiFiCheck = millis();
    reconnectAttempts++;
    
    Serial.print("[WiFi] Connection lost (attempt ");
    Serial.print(reconnectAttempts);
    Serial.println("), attempting recovery...");
    
    // ========== Try to Reconnect with Saved Credentials ==========
    char ssid[33];
    char password[64];
    
    if (loadWiFiCredentials(ssid, password)) {
      // Try single connection attempt (will retry on next interval)
      if (connectWiFi(ssid, password, 1)) {
        reconnectAttempts = 0;  // Reset counter on success
        
        // ========== Reconnect MQTT After WiFi Recovery ==========
        if (!mqtt.connected()) {
          Serial.println("[System] WiFi recovered, reconnecting MQTT...");
          connectMqtt();
        }
      }
    } else {
      // No credentials - restart BLE provisioning
      if (!isBLEProvisioningActive()) {
        Serial.println("[WiFi] No credentials - starting BLE provisioning...");
        initBLEProvisioning();
        reconnectAttempts = 0;
      }
    }
    return;  // Skip rest of loop while reconnecting
  }
  
  // ========== Reset Reconnect Counter When Connected ==========
  if (WiFi.status() == WL_CONNECTED && reconnectAttempts > 0) {
    reconnectAttempts = 0;
  }
  
  // ========== If WiFi Not Connected, Wait ==========
  if (WiFi.status() != WL_CONNECTED) {
    delay(100);
    return;
  }
  
  // ========== Normal Operations ==========
  // Update timer countdown
  updateTimer();
  
  // Publish WiFi state periodically (every 30 seconds)
  static uint32_t lastWiFiUpdate = 0;
  if (millis() - lastWiFiUpdate > WIFI_STATE_INTERVAL) {
    lastWiFiUpdate = millis();
    if (mqtt.connected()) {
      publishWiFiState();
    }
  }
  
  // Read and publish temperature (every 60 seconds)
  static uint32_t lastTempUpdate = 0;
  if (millis() - lastTempUpdate > TEMP_PUBLISH_INTERVAL) {
    lastTempUpdate = millis();
    currentTemperature = readTemperature();
    if (mqtt.connected()) {
      publishTemperature();
    }
  }
  
  // ========== MQTT Reconnection ==========
  if (!mqtt.connected()) {
    Serial.println("[MQTT] Connection lost, reconnecting...");
    connectMqtt();
  }

  // ========== Process MQTT Messages ==========
  mqtt.loop();  // Must be called regularly to keep connection alive
}
```

**WiFi Reconnection Strategy:**

1. **Check every 10 seconds** - not too aggressive
2. **Single retry per check** - spreads attempts over time
3. **Track attempt count** - for diagnostics
4. **Reset counter on success** - clean state
5. **Reconnect MQTT after WiFi recovery** - restore full functionality

**Why Not Aggressive Reconnection?**
- Saves battery/power
- Reduces log spam
- Allows other tasks to run
- Network issues often take minutes to resolve (router reboot)

---

## MQTT WiFi Clear Command

The system also supports clearing WiFi credentials remotely via MQTT:

```cpp
// In MQTT callback handler:
if (topic == TOPIC_WIFI_CLEAR) {
  Serial.println("[MQTT] WiFi clear command received from dashboard");
  
  // ========== Publish Disconnected State ==========
  mqtt.publish(TOPIC_WIFI_STATE, "{\"status\":\"disconnected\"}", true);
  delay(100);  // Let message send
  
  // ========== Disconnect MQTT ==========
  mqtt.disconnect();
  
  // ========== Disconnect WiFi and Erase Credentials ==========
  WiFi.disconnect(true /*wifioff*/, true /*erasePersistent*/);
  clearWiFiCredentials();
  
  Serial.println("[WiFi] Credentials erased. Restarting in 2 seconds...");
  delay(2000);
  
  // ========== Restart ESP32 ==========
  // Cleanly enters BLE provisioning mode on boot
  ESP.restart();
}
```

**Why Restart After Clearing?**
- Clean state for BLE provisioning
- Avoids complex state management in running code
- Ensures all WiFi/BLE handles are properly released

---

## Summary

### Key Features

1. **BLE Provisioning (Primary)**
   - User-friendly Web Bluetooth API
   - No network switching required
   - WiFi network scanning
   - Real-time status notifications

2. **Persistent Credentials**
   - NVS storage survives reboots
   - Automatic reconnection after power loss
   - Credentials preserved on temporary network failures

3. **Automatic Retry Logic**
   - 3 attempts with 5-second delays
   - Handles temporary network issues
   - Reconnects automatically when network returns

4. **Remote Management**
   - Clear credentials via MQTT
   - Clear credentials via BLE
   - No physical access required

5. **Resource Efficiency**
   - BLE stopped after provisioning (~30-50KB RAM saved)
   - Periodic checks instead of busy-waiting
   - Minimal flash wear with NVS library

### Security Considerations

1. **Password Characteristic**
   - Write-only (no reading back)
   - Not logged to serial

2. **BLE Range**
   - ~10-30 meters typical
   - Requires physical proximity
   - Harder to intercept than WiFi

3. **TLS/MQTTS**
   - WiFi credentials enable secure MQTT connection
   - Certificate validation for broker

### Troubleshooting Guide

| Issue | Possible Cause | Solution |
|-------|---------------|----------|
| BLE not discoverable | MAC address conflict | Check device name in serial log |
| WiFi won't connect | Wrong password | Clear credentials and re-provision |
| Connection drops frequently | Weak signal (< -70 dBm) | Move router closer or use extender |
| System won't enter BLE mode | Credentials saved but invalid | Send MQTT clear command or use BLE clear |
| Temperature not updating | Sensor disconnected | Check wiring on GPIO 21 |

### Flow Diagram

```
┌───────────────────────────────────────────────────────────────────────┐
│                         ESP32 BOOT                                     │
└───────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼
                     ┌─────────────────────────┐
                     │  Load credentials       │
                     │  from NVS               │
                     └─────────────────────────┘
                                  │
                    ┌─────────────┴──────────────┐
                    │                            │
            ✓ Credentials exist          ✗ No credentials
                    │                            │
                    ▼                            │
        ┌───────────────────────┐               │
        │ Try WiFi connection   │               │
        │ with 3 retries        │               │
        └───────────────────────┘               │
                    │                            │
          ┌─────────┴─────────┐                 │
          │                   │                 │
      Connected          Failed (3x)            │
          │                   │                 │
          │                   └─────────────────┘
          │                             │
          ▼                             ▼
  ┌────────────────┐        ┌──────────────────────────┐
  │ Sync NTP       │        │ Start BLE advertising    │
  │ Setup MQTT     │        │ "Controlador Pool-XXXX"  │
  │ Connect MQTT   │        └──────────────────────────┘
  │ Normal ops     │                    │
  └────────────────┘                    ▼
          │                  ┌──────────────────────────┐
          │                  │ Dashboard connects       │
          │                  │ Sends SSID + Password    │
          │                  └──────────────────────────┘
          │                             │
          │                             ▼
          │                  ┌──────────────────────────┐
          │                  │ Stop BLE                 │
          │                  │ Try WiFi connection      │
          │                  └──────────────────────────┘
          │                             │
          │                   ┌─────────┴──────────┐
          │                   │                    │
          │              Connected             Failed
          │                   │                    │
          │                   ▼                    ▼
          │        ┌──────────────────┐  ┌─────────────────┐
          │        │ Save to NVS      │  │ Restart BLE     │
          │        │ Sync NTP         │  │ Ask user retry  │
          │        │ Setup MQTT       │  └─────────────────┘
          │        │ Connect MQTT     │
          │        └──────────────────┘
          │                   │
          └───────────────────┘
                      │
                      ▼
          ┌──────────────────────────┐
          │    MAIN LOOP             │
          │                          │
          │  • Update timer          │
          │  • Publish WiFi state    │
          │  • Read temperature      │
          │  • Check WiFi status     │
          │  • Reconnect if needed   │
          │  • Process MQTT messages │
          └──────────────────────────┘
```

---

## Conclusion

The BLE provisioning system provides a modern, user-friendly way to configure WiFi credentials on the ESP32 Pool Controller. It combines:

- **Ease of use** - Web Bluetooth API in the dashboard
- **Reliability** - Automatic retry and reconnection logic
- **Efficiency** - BLE stopped after provisioning to save resources
- **Flexibility** - Remote credential management via MQTT

This approach eliminates the need for users to switch WiFi networks or configure access points manually, making the device more accessible to non-technical users.
