/**
 * @file ble_provisioning.h
 * @brief BLE Provisioning for WiFi credentials via Web Bluetooth API
 * 
 * This module handles WiFi credential provisioning via Bluetooth Low Energy (BLE).
 * The ESP32 advertises a BLE service that can be discovered by the web dashboard.
 * Users can send WiFi credentials from the dashboard without switching networks.
 * 
 * Flow:
 * 1. ESP32 boots and starts BLE advertising (if no WiFi credentials)
 * 2. Dashboard uses Web Bluetooth API to scan and connect
 * 3. Dashboard writes WiFi SSID and password to BLE characteristics
 * 4. ESP32 saves credentials to NVS and attempts WiFi connection
 * 5. BLE is disabled after successful WiFi connection (saves power)
 */

#ifndef BLE_PROVISIONING_H
#define BLE_PROVISIONING_H

#include <Arduino.h>

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
 * Check if BLE provisioning is active
 * @return true if BLE is running, false otherwise
 */
bool isBLEProvisioningActive();

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
 * @return JSON string with network list: [{"ssid":"NETWORK1","rssi":-50,"open":false},...]
 * Call this in response to a scan request from the dashboard
 */
String scanWiFiNetworks();

/**
 * Clear the received credentials flag
 * Call this after successfully connecting to WiFi
 */
void clearBLECredentials();

#endif // BLE_PROVISIONING_H
