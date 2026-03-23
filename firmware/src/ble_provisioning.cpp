/**
 * @file ble_provisioning.cpp
 * @brief BLE Provisioning implementation for ESP32 Pool Controller
 */

#include "ble_provisioning.h"
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <WiFi.h>

// ==================== BLE UUIDs ====================
// Custom UUIDs for Pool Controller WiFi Provisioning Service
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define SSID_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define PASSWORD_CHAR_UUID  "cba1d466-344c-4be3-ab3f-189f80dd7518"
#define STATUS_CHAR_UUID    "8d8218b6-97bc-4527-a8db-13094ac06b1d"
#define NETWORKS_CHAR_UUID  "fa87c0d0-afac-11de-8a39-0800200c9a66"  // WiFi networks scan result

// ==================== Global BLE Objects ====================
static NimBLEServer* pServer = nullptr;
static NimBLECharacteristic* pSSIDCharacteristic = nullptr;
static NimBLECharacteristic* pPasswordCharacteristic = nullptr;
static NimBLECharacteristic* pStatusCharacteristic = nullptr;
static NimBLECharacteristic* pNetworksCharacteristic = nullptr;

// ==================== State Variables ====================
static bool bleActive = false;
static bool newCredentialsReceived = false;
static String receivedSSID = "";
static String receivedPassword = "";
static bool deviceConnected = false;

// ==================== BLE Callbacks ====================

/**
 * Server callback - handles client connect/disconnect events
 */
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer) {
    deviceConnected = true;
    Serial.println("[BLE] Client connected");
    
    // Update status characteristic
    if (pStatusCharacteristic) {
      pStatusCharacteristic->setValue("connected");
      pStatusCharacteristic->notify();
    }
  }

  void onDisconnect(NimBLEServer* pServer) {
    deviceConnected = false;
    Serial.println("[BLE] Client disconnected");
    
    // Restart advertising so others can connect
    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] Advertising restarted");
  }
};

/**
 * Characteristic callbacks - handles write events for WiFi credentials
 */
class CharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string uuid = pCharacteristic->getUUID().toString();
    std::string value = pCharacteristic->getValue();
    
    if (uuid == SSID_CHAR_UUID) {
      receivedSSID = String(value.c_str());
      Serial.print("[BLE] SSID received: ");
      Serial.println(receivedSSID);
      
      // Update status
      if (pStatusCharacteristic) {
        pStatusCharacteristic->setValue("ssid_received");
        pStatusCharacteristic->notify();
      }
    } 
    else if (uuid == PASSWORD_CHAR_UUID) {
      receivedPassword = String(value.c_str());
      Serial.print("[BLE] Password received (");
      Serial.print(receivedPassword.length());
      Serial.println(" chars)");
      
      // Update status
      if (pStatusCharacteristic) {
        pStatusCharacteristic->setValue("password_received");
        pStatusCharacteristic->notify();
      }
      
      // Both credentials received
      if (receivedSSID.length() > 0 && receivedPassword.length() > 0) {
        newCredentialsReceived = true;
        Serial.println("[BLE] ✓ WiFi credentials complete");
        
        if (pStatusCharacteristic) {
          pStatusCharacteristic->setValue("credentials_ready");
          pStatusCharacteristic->notify();
        }
      }
    }
    else if (uuid == NETWORKS_CHAR_UUID) {
      // Trigger WiFi scan when client writes to networks characteristic
      Serial.println("[BLE] Networks scan triggered via write");
      String json = scanWiFiNetworks();
      
      // Update the characteristic with scan results
      pCharacteristic->setValue((uint8_t*)json.c_str(), json.length());
      Serial.print("[BLE] Networks characteristic updated, length: ");
      Serial.println(json.length());
      
      // Notify client that new data is available
      pCharacteristic->notify();
    }
  }
  
  void onRead(NimBLECharacteristic* pCharacteristic) {
    std::string uuid = pCharacteristic->getUUID().toString();
    
    // Log when networks characteristic is read
    if (uuid == NETWORKS_CHAR_UUID) {
      Serial.println("[BLE] Networks characteristic read");
    }
  }
};

// Forward declaration for scanWiFiNetworks
String scanWiFiNetworks();

// ==================== Public Functions ====================

void initBLEProvisioning() {
  Serial.println("[BLE] Initializing BLE provisioning...");
  
  // Generate device name with MAC address suffix
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char deviceName[32];
  // Add version suffix to break cached GATT on clients
  snprintf(deviceName, sizeof(deviceName), "Controlador Smart Pool-%02X%02X-v2", mac[4], mac[5]);
  
  Serial.print("[BLE] Device name: ");
  Serial.println(deviceName);
  
  // Initialize NimBLE
  NimBLEDevice::init(deviceName);
  
  // Create BLE Server
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());
  
  // Create BLE Service
  NimBLEService* pService = pServer->createService(SERVICE_UUID);
  
  // Create SSID Characteristic (Read/Write)
  pSSIDCharacteristic = pService->createCharacteristic(
    SSID_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
  );
  pSSIDCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pSSIDCharacteristic->setValue(""); // Initial empty value
  
  // Create Password Characteristic (Write only for security)
  pPasswordCharacteristic = pService->createCharacteristic(
    PASSWORD_CHAR_UUID,
    NIMBLE_PROPERTY::WRITE
  );
  pPasswordCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pPasswordCharacteristic->setValue("");
  
  // Create Status Characteristic (Read/Notify)
  pStatusCharacteristic = pService->createCharacteristic(
    STATUS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  pStatusCharacteristic->setValue("waiting");
  
  // Create Networks Characteristic (Read/Write - write triggers scan, read returns JSON)
  pNetworksCharacteristic = pService->createCharacteristic(
    NETWORKS_CHAR_UUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::NOTIFY
  );
  pNetworksCharacteristic->setCallbacks(new CharacteristicCallbacks());
  pNetworksCharacteristic->setValue("[]"); // Initial empty list
  
  // Start the service
  pService->start();
  
  // Start advertising
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  // Apple connection parameter
  pAdvertising->setMaxPreferred(0x12);
  
  NimBLEDevice::startAdvertising();
  
  bleActive = true;
  
  Serial.println("[BLE] ✓ Provisioning service started");
  Serial.println("[BLE] Waiting for dashboard connection...");
  Serial.print("[BLE] Service UUID: ");
  Serial.println(SERVICE_UUID);
}

void stopBLEProvisioning() {
  if (!bleActive) return;
  
  Serial.println("[BLE] Stopping provisioning service...");
  
  NimBLEDevice::stopAdvertising();
  
  // deinit() automatically disconnects all clients
  NimBLEDevice::deinit(true);
  
  bleActive = false;
  deviceConnected = false;
  pServer = nullptr;
  
  Serial.println("[BLE] ✓ Provisioning stopped");
}

bool isBLEProvisioningActive() {
  return bleActive;
}

bool hasNewWiFiCredentials() {
  return newCredentialsReceived;
}

bool getBLEWiFiSSID(char* ssid) {
  if (receivedSSID.length() == 0) return false;
  
  strncpy(ssid, receivedSSID.c_str(), 32);
  ssid[32] = '\0';
  return true;
}

bool getBLEWiFiPassword(char* password) {
  if (receivedPassword.length() == 0) return false;
  
  strncpy(password, receivedPassword.c_str(), 63);
  password[63] = '\0';
  return true;
}

void clearBLECredentials() {
  newCredentialsReceived = false;
  receivedSSID = "";
  receivedPassword = "";
}

/**
 * Scan available WiFi networks and return JSON array
 * @return JSON string with network list: [{"ssid":"NETWORK1","rssi":-50,"open":false},...]
 */
String scanWiFiNetworks() {
  Serial.println("[BLE] Scanning WiFi networks...");
  
  // Properly reset WiFi driver state for BLE coexistence
  // After WiFi.disconnect(true, true) the driver may be in inconsistent state
  WiFi.mode(WIFI_OFF);
  delay(100);
  WiFi.mode(WIFI_STA);
  delay(200);  // Give WiFi radio time to initialize after BLE is active
  
  // Perform WiFi scan
  int numNetworks = WiFi.scanNetworks();
  
  if (numNetworks == 0 || numNetworks == -1) {
    Serial.println("[BLE] No networks found or scan failed");
    Serial.print("[BLE] WiFi status code: ");
    Serial.println(WiFi.status());
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    return "[]";
  }
  
  Serial.print("[BLE] Found ");
  Serial.print(numNetworks);
  Serial.println(" networks");
  
  // Build JSON array
  String json = "[";
  int networkCount = 0;
  
  for (int i = 0; i < numNetworks; i++) {
    String ssid = WiFi.SSID(i);
    int rssi = WiFi.RSSI(i);
    uint8_t encryption = WiFi.encryptionType(i);
    bool open = (encryption == WIFI_AUTH_OPEN);
    
    // Skip empty SSIDs
    if (ssid.length() == 0) continue;
    
    // Build network entry
    String entry = "{\"ssid\":\"";
    entry += ssid;
    entry += "\",\"rssi\":";
    entry += String(rssi);
    entry += ",\"open\":";
    entry += (open ? "true" : "false");
    entry += "}";
    
    // Check if adding this entry would exceed safe BLE MTU (~400 bytes for reliable transmission)
    int projectedSize = json.length() + entry.length() + (networkCount > 0 ? 1 : 0) + 1; // +1 for comma, +1 for ]
    if (projectedSize > 400) {
      Serial.println("[BLE] Network list too large, stopping here");
      break;
    }
    
    if (networkCount > 0) json += ",";
    json += entry;
    networkCount++;
  }
  
  json += "]";
  
  // Clean up
  WiFi.scanDelete();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  
  Serial.print("[BLE] JSON size: ");
  Serial.print(json.length());
  Serial.println(" bytes");
  Serial.print("[BLE] JSON: ");
  Serial.println(json);
  
  return json;
}
