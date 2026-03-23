# Device WiFi Provisioning Guide

This guide covers WiFi provisioning for the ESP32 Pool Controller using BLE provisioning from the web dashboard.

## Connection Priority at Boot

On every boot, firmware uses this order:
1. Load WiFi credentials from NVS saved from a previous successful provisioning.
2. If NVS credentials fail, try fallback credentials from `firmware/include/secrets.h` in this order:
   - `WIFI_SSID` / `WIFI_PASS`
   - `WIFI_SSID_2` / `WIFI_PASS_2`
   - `WIFI_SSID_3` / `WIFI_PASS_3`
3. If all attempts fail, start BLE provisioning.

If a fallback network connects, those credentials are saved to NVS automatically. That means the next boot usually connects immediately without running provisioning again.

## Quick Reference

| Device Type | Method | Browser | Setup Time |
|-------------|--------|---------|-----------|
| Android Phone | BLE | Chrome/Edge/Opera | ~2 min |
| Windows/macOS | BLE | Chrome/Edge/Opera | ~2 min |

## BLE Provisioning

### How It Works

The ESP32 advertises a BLE service. The dashboard connects with Web Bluetooth, scans nearby WiFi networks, and writes the selected SSID and password to the device. After both values arrive, the firmware stops BLE, calls `WiFi.begin(...)`, and saves the credentials to NVS if the connection succeeds.

### Prerequisites

- Chrome, Edge, or Opera with Web Bluetooth support
- Bluetooth capability on your device
- WiFi network name and password

### First-Time Setup

1. Power on the ESP32 device.
2. Open the web dashboard in a supported browser.
3. Click the WiFi provisioning button.
4. Click `Buscar dispositivo y redes`.
5. In the browser picker, select the ESP32 device named `Controlador Smart Pool-XXXX-v2`.
6. Choose a WiFi network from the scanned list or enter the SSID manually.
7. Enter the WiFi password.
8. Click `Conectar`.
9. Wait for the ESP32 to connect and complete initialization.

### Performance

| Step | Duration |
|------|----------|
| BLE discovery | 1-2 seconds |
| Network scan | 2-3 seconds |
| Credential transmission | <1 second |
| WiFi connection | 5-15 seconds |
| Total | ~10-20 seconds |

### BLE Service Details

Primary service UUID: `4fafc201-1fb5-459e-8fcc-c5c9c331914b`

| Characteristic | UUID | Direction | Purpose |
|---|---|---|---|
| SSID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` | Write | Send network name |
| Password | `cba1d466-344c-4be3-ab3f-189f80dd7518` | Write | Send network password |
| Networks | `fa87c0d0-afac-11de-8a39-0800200c9a66` | Read/Write | Trigger scan and read network list |
| Status | `8d8218b6-97bc-4527-a8db-13094ac06b1d` | Read/Notify | Provisioning status |

### Status Values

| Status | Meaning |
|--------|---------|
| `waiting` | Device is advertising and ready |
| `connected` | BLE client connected |
| `ssid_received` | SSID written successfully |
| `password_received` | Password written successfully |
| `credentials_ready` | Both credentials received; firmware is ready to connect |

## Troubleshooting

### Device does not appear in the browser picker

Check the following:
- Bluetooth is enabled.
- You are using Chrome, Edge, or Opera.
- The ESP32 is powered on.
- The device is within BLE range.
- The ESP32 is in provisioning mode because no saved WiFi credentials could connect.

### `GATT Server disconnected`

This usually means the browser cached stale BLE metadata or the ESP32 restarted its BLE service.

Try this:
1. Disconnect from the device in the dashboard.
2. Restart the ESP32.
3. Remove the saved Bluetooth device from the OS if necessary.
4. Retry provisioning.

### WiFi connection fails after sending credentials

Check the following:
- Password is correct and case-sensitive.
- SSID spelling matches exactly.
- The ESP32 is in range of the WiFi network.
- The access point is using a 2.4 GHz network that the ESP32 can join.
- The router is fully online if power was recently restored.

## Power-Failure Recovery

The ESP32 automatically recovers without re-provisioning in most cases:
1. Boot and load saved credentials from NVS.
2. Retry the last known WiFi credentials several times.
3. Keep those credentials even if the router is still booting.
4. Continue background reconnect attempts until WiFi returns.

If the saved credentials are no longer valid, the device falls back to BLE provisioning.

## Clearing WiFi Credentials

Use the existing MQTT clear command while the device is online.

After clearing credentials, the device returns to BLE provisioning mode on the next boot or reconnect cycle.

## Browser Compatibility

| Browser | Desktop | Android | iOS |
|---------|---------|---------|-----|
| Chrome | Yes | Yes | No |
| Edge | Yes | Yes | No |
| Opera | Yes | Yes | No |
| Safari | No | No | No |
| Firefox | No | No | No |

Recommendation: use Chrome for the most predictable Web Bluetooth behavior.

## Notes for Developers

- BLE and WiFi share the ESP32 radio, so the firmware resets WiFi state before scanning networks.
- The networks characteristic response is size-limited to stay within a reliable BLE payload.
- Credentials are buffered by the BLE module first; the actual WiFi connection attempt happens later in the main loop.

## References

- [Web Bluetooth API Documentation](https://developer.mozilla.org/en-US/docs/Web/API/Web_Bluetooth_API)
- [ESP32 BLE API](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/bluetooth/index.html)
- [ESP32 NVS Storage](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/storage/nvs_flash.html)
