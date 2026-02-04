# Pool Control System - Wiring Diagram v3.0

> 📋 **Note:** For a complete visual schematic, see [wiring_diagram.png](wiring_diagram.png)

## Hardware Components
- ESP32 DevKit V1
- 1× Dual-channel relay module (2× Songle SRD-05VDC-SL-C relays)
- 1× DS18B20 temperature sensor (waterproof)
- 1× 4.7kΩ resistor (pull-up for DS18B20 data line)
- 2× 10kΩ resistors (pull-down for GPIO 25 and GPIO 26)
- 24V DC power supply (5.5A for pump + valves)
- LM2596S buck converter (24V → 5V for ESP32)
- 2× SPDT manual override switches (optional)

---

## GPIO Pin Assignment

### Outputs (Relay Control)
```
GPIO 25 → VALVE_RELAY_PIN   (Relay IN1: 2× 24V electrovalves) + 10kΩ pull-down to GND
GPIO 26 → PUMP_RELAY_PIN    (Relay IN2: 220V pump)           + 10kΩ pull-down to GND
```

### Inputs (Sensors)
```
GPIO 4 → TEMP_SENSOR_PIN   (DS18B20 OneWire data line)      + 4.7kΩ pull-up to 3.3V
```

---

## Dual-Channel Relay Module

Most dual-channel relay modules have **built-in optocouplers** and don't require external transistor drivers.

### Module Pinout (Typical)
```
VCC  → ESP32 5V (or VIN)
GND  → ESP32 GND
IN1  → ESP32 GPIO 25 (Valve control) + [10kΩ to GND]
IN2  → ESP32 GPIO 26 (Pump control)  + [10kΩ to GND]

Relay 1 (IN1 - Valves):
  COM → 24V DC (+) from power supply
  NO  → NC valve (+) AND NO valve (-)  [see valve wiring below]
  NC  → NC valve (-) AND NO valve (+)  [see valve wiring below]

Relay 2 (IN2 - Pump):
  COM → 220V Hot (from circuit breaker)
  NO  → Pump motor hot wire
  NC  → Not used
```

### Relay Module Logic
- **Active LOW:** Most modules activate when GPIO = LOW (0V)
- **Active HIGH:** Some modules activate when GPIO = HIGH (3.3V/5V)
- Check your module's documentation or label (usually marked "H" or "L")

**Note:** The firmware uses `digitalWrite(pin, HIGH)` to activate relays, so use an **Active HIGH** module or invert the logic in code.

### Pump Control - Inverted Display Logic

⚠️ **Important:** The pump relay physical wiring requires **inverted logic** between firmware and dashboard display:

- **Firmware logic (correct):** Sends `"ON"` when relay should be activated, `"OFF"` when relay should be deactivated
- **Dashboard display (inverted):** Shows pump as **ON** when firmware sends `"OFF"`, and shows **OFF** when firmware sends `"ON"`

This inversion is implemented in the frontend code (`docs/js/app.js` in the `setPumpState()` function) to match the physical wiring requirements of the relay module and pump motor configuration.

**Why:** The relay wiring is configured so that when the relay is physically OFF, the pump should be considered ON from a user perspective, and vice versa.

---

## Valve Wiring (2 Electrovalves in Parallel)

The system uses **2× 24V electrovalves** (1× NC + 1× NO) wired to operate in opposite modes:

### Mode 1 (Cascada) - Relay LOW
```
Relay 1 NC contact closed:
  24V+ → Relay COM → Relay NC → NC valve (+) → NC valve (-) → GND
                               → NO valve (-) → NO valve (+) → 24V+
Result: NC valve OPEN, NO valve CLOSED
```

### Mode 2 (Eyectores) - Relay HIGH  
```
Relay 1 NO contact closed:
  24V+ → Relay COM → Relay NO → NC valve (-) → NC valve (+) → GND
                               → NO valve (+) → NO valve (-) → 24V+
Result: NC valve CLOSED, NO valve OPEN
```

**Wiring Schematic:**
```
        Relay 2
          COM ────── 24V+ (from power supply)
           │
    ┌──────┴──────┐
    │             │
   NC            NO
    │             │
    │             │
    └──┬───┐  ┌──┴───┐
       │   │  │      │
    NC Valve │  │  NO Valve
     (+) (-) │  │  (+) (-)
       │   │  │  │    │
       └───┼──┘  └────┼───► Both negatives to GND
           │           │
           └───────────┘
           (Cross-connected)
```

---

## DS18B20 Temperature Sensor

### Wiring (3-wire waterproof sensor)
```
Red    → ESP32 3.3V
Black  → ESP32 GND
Yellow → GPIO 4 + [4.7kΩ pull-up to 3.3V]
```

### Pull-up Resistor
```
ESP32 3.3V ──┬── DS18B20 VCC (red)
             │
         [4.7kΩ]
             │
             └── DS18B20 DATA (yellow) ── GPIO 4
```

**Important:** Use **4.7kΩ** (not 47kΩ) for reliable OneWire communication.

---

## Pull-Down Resistors (GPIO Boot Protection)

To prevent relays from activating randomly during ESP32 boot/reset, install pull-down resistors:

### Wiring
```
GPIO 25 ──[10kΩ]── GND  (Valve relay control)
GPIO 26 ──[10kΩ]── GND  (Pump relay control)
```

**Why needed:**
- During boot, ESP32 GPIO pins float (undefined state)
- Floating pins can trigger relay activation
- 10kΩ pull-down ensures GPIO stays LOW until firmware initializes
- Prevents pump/valves from turning on unexpectedly during power-up

**Installation:**
- Solder resistor between GPIO pin and GND rail on breadboard/PCB
- Or use resistor directly on relay module if it has dedicated pull-down pads

---

## Manual Override Switches (Optional)

Wire SPDT switches in **parallel** with ESP32 relays for manual control:

### Pump Manual Override
```
Manual Switch:
  Common → 220V Hot
  NO     → Pump motor hot (in parallel with Relay 1 NO)
  
Either ESP32 OR manual switch can turn on pump
```

### Valve Manual Override  
```
Two separate switches for Mode 1 / Mode 2:
  Mode 1 Switch → Connects 24V to NC valve circuit
  Mode 2 Switch → Connects 24V to NO valve circuit
  
Wired in parallel with Relay 2 contacts
```

---

## Power Distribution

### Main Power Supply Chain
```
220V AC ──► 24V DC Power Supply (5.5A)
            │
            ├──► Pump motor (when relay active)
            ├──► Electrovalves (1-2A typical)
            │
            └──► LM2596S Buck Converter
                 │
                 └──► 5V DC (2A)
                      │
                      ├──► ESP32 VIN (or 5V pin)
                      └──► Relay module VCC
```

### Grounding
```
All GND connections must be common:
  - ESP32 GND
  - Relay module GND  
  - DS18B20 GND (black)
  - 24V power supply (-)
  - 5V buck converter (-)
```

---

## Complete System Diagram (ASCII)

```
                    ┌──────────────────────────────────┐
                    │       ESP32 DevKit V1            │
                    │                                  │
   ┌────────────────┤ GPIO 25 (Valves) + [10kΩ↓GND]   │
   │  ┌─────────────┤ GPIO 26 (Pump)   + [10kΩ↓GND]   │
   │  │  ┌──────────┤ GPIO 4 (Temp)    + [4.7kΩ↑3.3V] │
   │  │  │          │                                  │
   │  │  │          │ VIN/5V ◄── 5V from LM2596S       │
   │  │  │          │ GND ◄──────────┬── Common GND    │
   │  │  │          └────────────────┼─────────────────┘
   │  │  │                           │
   │  │  │          ┌────────────────┴─────────────┐
   │  │  │          │  Dual Relay Module           │
   │  │  │          │  VCC ◄── 5V                  │
   │  └──┼─────────►│  IN1 (Valve Relay)           │
   │     └─────────►│  IN2 (Pump Relay)            │
   │                │  GND ◄── GND                 │
   │                └──┬───────────┬───────────────┘
   │                   │           │
   │                   │           │ Relay 1 (IN1 - Valves)
   │                   │           ├── COM ◄── 24V+
   │                   │           ├── NO ──► NC valve(-) + NO valve(+)
   │                   │           └── NC ──► NC valve(+) + NO valve(-)
   │                   │
   │                   │ Relay 2 (IN2 - Pump)
   │                   ├── COM ◄── 220V Hot
   │                   ├── NO ──► Pump motor hot
   │                   └── NC (not used)
   │
   └► DS18B20 (yellow) with 4.7kΩ pull-up to 3.3V
      DS18B20 red → 3.3V
      DS18B20 black → GND


Pull-Down Resistors (prevent relay activation during ESP32 boot):
  GPIO 16 ──[10kΩ]── GND
  GPIO 19 ──[10kΩ]── GND

Power Supply Chain:
220V AC → 24V DC (5.5A) ─┬─► Pump (via relay)
                         ├─► Valves (NC + NO)
                         └─► LM2596S → 5V (2A) ─┬─► ESP32
                                                 └─► Relay module
```

---

## Safety Considerations

1. **High Voltage Isolation:**
   - Never connect ESP32 GPIO directly to 220V
   - Use proper gauge wire for pump motor (14 AWG minimum)
   - Install GFCI/RCD protection on pump circuit

2. **Relay Ratings:**
   - Songle SRD-05VDC-SL-C rated for 10A @ 250VAC
   - Verify pump motor current < 10A continuous
   - 24V valve current typically 1-2A (well within limits)

3. **Enclosure:**
   - Use IP65-rated waterproof enclosure for outdoor installation
   - Separate compartments for high-voltage (220V) and low-voltage (5V/24V)
   - Proper cable glands for all external connections
   - Mount relay module securely to prevent vibration damage

4. **Grounding:**
   - Common ground for all DC circuits (ESP32, relays, sensors, power supplies)
   - Pump motor chassis must be grounded to AC earth
   - Use 3-wire cable with ground for all AC connections

5. **Fusing:**
   - Main pump circuit: 15A circuit breaker
   - 24V DC supply: 10A fuse
   - 5V buck converter output: 3A fuse (optional)

6. **Waterproofing:**
   - DS18B20 must be in waterproof stainless steel probe
   - Seal all wire entries with silicone or cable glands
   - Mount enclosure above potential water line

---

## Installation Notes

1. **Pre-installation Testing:**
   - Test ESP32 + relay module on bench with LEDs before connecting loads
   - Verify MQTT connectivity and dashboard control
   - Check DS18B20 temperature readings

2. **Wiring Checklist:**
   - ✅ All GND connections common
   - ✅ Relay module polarity correct (VCC/GND)
   - ✅ GPIO 18/19 not swapped
   - ✅ DS18B20 pull-up resistor installed (4.7kΩ to 3.3V)
   - ✅ Manual override switches wired in parallel (if used)
   - ✅ Pump motor ground wire connected

3. **Power Supply Verification:**
   - Measure 24V DC output under load (should be 23-25V)
   - Verify LM2596S output = 5.0-5.2V DC
   - Check ESP32 voltage at VIN = 5V (or 3.3V at 3.3V pin)

4. **Relay Testing:**
   - Listen for relay click when GPIO activates
   - Verify contacts close with multimeter (continuity test)
   - Check for proper Active HIGH/LOW behavior

5. **Documentation:**
   - Label all wires with tags (P+ = pump hot, V+ = valve 24V+, etc.)
   - Take photos of completed wiring
   - Note pump motor current draw (measure with clamp meter)

---

## Troubleshooting

| Symptom | Likely Cause | Solution |
|---------|--------------|----------|
| Relay clicks but pump doesn't start | Manual override switch open | Check switch position or bypass for testing |
| ESP32 resets when relay activates | Insufficient power supply current | Upgrade to 3A+ power supply, check buck converter |
| Temperature reads -127°C | DS18B20 not connected or wrong pin | Verify GPIO 4, check pull-up resistor, test sensor |
| Temperature erratic/drops out | 47kΩ resistor (too high) | Replace with 4.7kΩ resistor |
| Relay doesn't click at all | GPIO not configured or wrong logic | Check firmware, verify Active HIGH/LOW setting |
| Valve stuck in one mode | Relay contacts welded or valve failure | Test relay with multimeter, check valve power |
| WiFi disconnects frequently | Weak signal or power issue | Move router closer, check 5V supply stability |
| MQTT connection fails | Wrong credentials or firewall | Verify secrets.h, check HiveMQ Cloud console |

---

## Maintenance

### Weekly
- Check dashboard connectivity
- Verify temperature readings are reasonable
- Test manual override switches (if installed)

### Monthly  
- Inspect relay module for signs of overheating or burning
- Check all wire connections are tight
- Verify pump motor current draw hasn't increased
- Clean enclosure vents/filters

### Annually
- Replace relay module if >100,000 cycles (pump runs daily)
- Test GFCI/RCD breaker
- Inspect all wire insulation for damage
- Update firmware if security patches available

---

## Upgrade Path

### Future Enhancements
- Add 12V LED strip control (GPIO 22 + relay or MOSFET)
- Add water level sensor (GPIO 36/39 ADC)
- Add flow meter for pump runtime verification  
- Add second temperature sensor for ambient air
- Implement OTA (Over-The-Air) firmware updates

---

## Resources

- [ESP32 Pinout Reference](https://randomnerdtutorials.com/esp32-pinout-reference-gpios/)
- [DS18B20 Datasheet](https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf)
- [Songle SRD-05VDC-SL-C Datasheet](https://components101.com/switches/5v-relay-pinout-working-datasheet)
- [LM2596 Buck Converter Guide](https://www.ti.com/lit/ds/symlink/lm2596.pdf)
- [HiveMQ Cloud MQTT Broker](https://www.hivemq.com/mqtt-cloud-broker/)

---

**Document Version:** v3.0  
**Last Updated:** December 30, 2025  
**Hardware Revision:** Dual-relay standard control (no latching, no feedback sensors)
