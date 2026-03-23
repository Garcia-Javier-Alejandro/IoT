/**
 * Controlador Smart Pool - BLE Provisioning Module
 * Web Bluetooth API integration for WiFi credential provisioning
 * 
 * Usage:
 * 1. Include this script in your dashboard
 * 2. Call ESP32BLEProvisioning.provision() when user clicks "Add Device"
 * 3. Handle success/error callbacks
 */

const ESP32BLEProvisioning = {
  // BLE Service & Characteristic UUIDs (must match firmware)
  SERVICE_UUID: '4fafc201-1fb5-459e-8fcc-c5c9c331914b',
  SSID_CHAR_UUID: 'beb5483e-36e1-4688-b7f5-ea07361b26a8',
  PASSWORD_CHAR_UUID: 'cba1d466-344c-4be3-ab3f-189f80dd7518',
  STATUS_CHAR_UUID: '8d8218b6-97bc-4527-a8db-13094ac06b1d',
  NETWORKS_CHAR_UUID: 'fa87c0d0-afac-11de-8a39-0800200c9a66',

  // State
  device: null,
  server: null,
  service: null,
  ssidCharacteristic: null,
  passwordCharacteristic: null,
  statusCharacteristic: null,
  networksCharacteristic: null,

  /**
   * Check if Web Bluetooth is supported
   * @returns {boolean} true if supported
   */
  isSupported() {
    if (!navigator.bluetooth) {
      console.error('[BLE] Web Bluetooth API not supported in this browser');
      return false;
    }
    return true;
  },

  /**
   * Scan for ESP32 Pool devices and connect
   * Shows browser's device picker UI
   * @returns {Promise<boolean>} true if connected successfully
   */
  async connect() {
    if (!this.isSupported()) {
      throw new Error('Web Bluetooth not supported. Use Chrome, Edge, or Opera.');
    }

    try {
      console.log('[BLE] Scanning for Smart Pool devices...');

      // Request device with our service UUID filter
      this.device = await navigator.bluetooth.requestDevice({
        filters: [
          { namePrefix: 'Controlador Smart Pool' }
        ],
        optionalServices: [this.SERVICE_UUID]
      });

      console.log(`[BLE] Found device: ${this.device.name}`);

      // Connect to GATT server
      console.log('[BLE] Connecting to GATT server...');
      this.server = await this.device.gatt.connect();
      console.log('[BLE] ✓ Connected to GATT server');

      // Get service
      this.service = await this.server.getPrimaryService(this.SERVICE_UUID);
      console.log('[BLE] ✓ Got provisioning service');

      // Get characteristics
      this.ssidCharacteristic = await this.service.getCharacteristic(this.SSID_CHAR_UUID);
      this.passwordCharacteristic = await this.service.getCharacteristic(this.PASSWORD_CHAR_UUID);
      this.networksCharacteristic = await this.service.getCharacteristic(this.NETWORKS_CHAR_UUID);
      this.statusCharacteristic = await this.service.getCharacteristic(this.STATUS_CHAR_UUID);
      
      console.log('[BLE] ✓ Got all characteristics');

      // Subscribe to status notifications
      await this.statusCharacteristic.startNotifications();
      this.statusCharacteristic.addEventListener('characteristicvaluechanged', (event) => {
        const value = new TextDecoder().decode(event.target.value);
        console.log(`[BLE] Status update: ${value}`);
      });

      return true;
    } catch (error) {
      console.error('[BLE] Connection error:', error);
      this.cleanup();
      throw error;
    }
  },

  /**
   * Scan for available WiFi networks
   * Request scan from ESP32 and get results via networks characteristic
   * @returns {Promise<Array>} Array of networks: [{ssid, rssi, open}, ...]
   */
  async scanNetworks() {
    if (!this.server || !this.server.connected) {
      throw new Error('Not connected to device. Call connect() first.');
    }

    if (!this.networksCharacteristic) {
      throw new Error('Networks characteristic not found.');
    }

    try {
      console.log('[BLE] Triggering network scan...');
      
      // Write "scan" to trigger the ESP32 to perform a WiFi scan
      const encoder = new TextEncoder();
      await this.networksCharacteristic.writeValue(encoder.encode('scan'));
      
      console.log('[BLE] Scan request sent, waiting for results...');
      
      // Wait for ESP32 to complete scan (2-3 seconds for WiFi scan)
      await new Promise(resolve => setTimeout(resolve, 3000));
      
      // Read the networks characteristic to get scan results
      const value = await this.networksCharacteristic.readValue();
      let json = new TextDecoder().decode(value);
      
      console.log('[BLE] Raw data length:', value.byteLength);
      console.log('[BLE] Raw data:', json);
      
      // Clean the JSON string - trim whitespace and remove any trailing garbage
      json = json.trim();
      
      // Find the end of valid JSON (last closing bracket)
      const lastBracket = json.lastIndexOf(']');
      if (lastBracket !== -1) {
        json = json.substring(0, lastBracket + 1);
      }
      
      console.log('[BLE] Cleaned JSON:', json);
      
      const networks = JSON.parse(json);
      console.log('[BLE] Parsed networks:', networks);
      return networks.sort((a, b) => b.rssi - a.rssi); // Sort by signal strength
    } catch (error) {
      console.error('[BLE] Scan error:', error);
      throw error;
    }
  },

  /**
   * Send WiFi credentials to ESP32
   * @param {string} ssid - WiFi network name
   * @param {string} password - WiFi password
   * @returns {Promise<boolean>} true if sent successfully
   */
  async sendCredentials(ssid, password) {
    if (!this.server || !this.server.connected) {
      throw new Error('Not connected to device. Call connect() first.');
    }

    try {
      console.log(`[BLE] Sending SSID: ${ssid}`);
      const ssidEncoder = new TextEncoder();
      await this.ssidCharacteristic.writeValue(ssidEncoder.encode(ssid));
      console.log('[BLE] ✓ SSID sent');

      // Small delay between writes
      await new Promise(resolve => setTimeout(resolve, 100));

      console.log('[BLE] Sending password...');
      const passwordEncoder = new TextEncoder();
      await this.passwordCharacteristic.writeValue(passwordEncoder.encode(password));
      console.log('[BLE] ✓ Password sent');

      return true;
    } catch (error) {
      console.error('[BLE] Error sending credentials:', error);
      throw error;
    }
  },

  /**
   * Disconnect from device
   */
  disconnect() {
    if (this.device && this.device.gatt.connected) {
      this.device.gatt.disconnect();
      console.log('[BLE] Disconnected');
    }
    this.cleanup();
  },

  /**
   * Clean up state
   */
  cleanup() {
    this.device = null;
    this.server = null;
    this.service = null;
    this.ssidCharacteristic = null;
    this.passwordCharacteristic = null;
    this.statusCharacteristic = null;
  },

    /**
   * Complete provisioning flow (high-level API)
   * @param {string} ssid - WiFi network name
   * @param {string} password - WiFi password
   * @param {Object} callbacks - Optional callbacks { onProgress, onSuccess, onError }
   * @returns {Promise<void>}
   */
  async provision(ssid, password, callbacks = {}) {
    const { onProgress, onSuccess, onError } = callbacks;

    try {
      // Step 1: Connect to device (if not already connected)
      if (!this.server || !this.server.connected) {
        if (onProgress) onProgress('Buscando dispositivo ESP32...');
        await this.connect();
      } else {
        if (onProgress) onProgress('Usando conexión existente...');
      }

      // Step 2: Send credentials
      if (onProgress) onProgress('Enviando credenciales WiFi...');
      await this.sendCredentials(ssid, password);

      // Step 3: ESP32 will disconnect BLE after receiving credentials
      // We consider this a success - the ESP32 is now connecting to WiFi
      if (onProgress) onProgress('¡Credenciales enviadas! ESP32 conectando a WiFi...');

      // Give ESP32 a moment to start WiFi connection, then disconnect our side
      await new Promise(resolve => setTimeout(resolve, 1000));

      // Disconnect (ESP32 might have already disconnected)
      try {
        this.disconnect();
      } catch (e) {
        // Ignore disconnect errors - ESP32 may have already disconnected
        console.log('[BLE] Device already disconnected (expected)');
      }

      if (onProgress) onProgress('¡Configuración completada!');
      if (onSuccess) onSuccess();

      console.log('[BLE]  Provisioning completed successfully');
    } catch (error) {
      console.error('[BLE] Provisioning failed:', error);
      
      // Clean up on error
      try {
        this.disconnect();
      } catch (e) {
        // Ignore cleanup errors
      }
      
      if (onError) onError(error);
      throw error;
    }
  }
};

// Export for use in modules (optional)
if (typeof module !== 'undefined' && module.exports) {
  module.exports = ESP32BLEProvisioning;
}

