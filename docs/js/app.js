/**
 * ==================== Pool Control Application Module ====================
 * Main application controller for simplified dashboard
 * 
 * Features:
 * - MQTT broker connection with credential management
 * - Pump control (ON/OFF with visual feedback)
 * - Valve mode control (1=Cascada, 2=Eyectores)
 * - Timer functionality (countdown with auto-stop)
 * - Program scheduling integration (conflict detection)
 * - WiFi status monitoring with signal quality
 * - Real-time logging with expand/collapse UI
 * 
 * Dependencies:
 * - MQTTModule: MQTT client communication
 * - LogModule: Event logging and display
 * - ProgramasModule: Schedule management (optional)
 * - APP_CONFIG: Configuration (topics, broker URL, device ID)
 */

const AppModule = (() => {
  // ==================== Constants ====================
  const BUTTON_DEBOUNCE_MS = 1000;           // Prevent rapid button clicks
  const VALVE_SWITCH_DELAY_MS = 500;         // Delay between valve and pump commands
  const TIMER_UPDATE_INTERVAL = 1000;        // Update timer display every second
  const PROGRAMAS_UPDATE_INTERVAL = 60000;   // Update programas button every minute
  const SCREEN_TRANSITION_DELAY = 500;       // Delay before returning to main screen
  const MAX_PROGRAM_NAME_LENGTH = 12;        // Max characters for program name display
  const STORAGE_KEY_USER = 'mqtt_user';      // localStorage key for MQTT username
  const STORAGE_KEY_PASS = 'mqtt_pass';      // localStorage key for MQTT password
  
  // ==================== Credential Provider ====================
  /**
   * Get MQTT credentials for authentication
   * 
   * AUTHENTICATION ARCHITECTURE:
   * 
   * Current (Single-User Personal Pool):
   * - Reads from APP_CONFIG.MQTT_USER and MQTT_PASS (config.js)
   * - All dashboards connect with same credentials
   * - All users share same MQTT topics (e.g., devices/esp32-pool-01/*)
   * - Suitable for single-family/single-location deployment
   * 
   * Future Enhancement (If Multi-User Needed):
   * - User authenticates with personal account (username/password or OAuth)
   * - Backend API returns user-specific MQTT credentials
   * - Each user gets unique topic namespace (e.g., devices/{userId}/pool-01/*)
   * - Enables multi-user deployment with access control
   * - See docs/ARCHITECTURE.md for scaling roadmap
   * 
   * This abstraction allows future migration to multi-user without changing
   * the connection logic throughout the application.
   * 
   * @returns {Promise<{user: string, pass: string}>} MQTT credentials
   */
  async function getMQTTCredentials() {
    // Current: Single-user mode - shared credentials from config.js
    // Development/Testing: Use credentials from APP_CONFIG
    // Production: Replace with environment variables or secure backend endpoint
    
    if (!window.APP_CONFIG.MQTT_USER || !window.APP_CONFIG.MQTT_PASS) {
      throw new Error('MQTT credentials not configured in config.js');
    }
    
    return {
      user: window.APP_CONFIG.MQTT_USER,
      pass: window.APP_CONFIG.MQTT_PASS
    };
    
    /* FUTURE ENHANCEMENT: Multi-user authentication (if needed)
     * 
     * Implementation example:
     * 
     * 1. User logs in with their account credentials
     * 2. Backend validates user and generates/retrieves their MQTT credentials
     * 3. Backend returns: { mqttUser, mqttPass, deviceTopicPrefix }
     * 4. Dashboard uses these credentials to connect to MQTT
     * 5. All topics are prefixed with user's namespace
     * 
     * Benefits:
     * - Each user has isolated MQTT topics
     * - Users can't access other users' devices
     * - Fine-grained access control (read/write permissions)
     * - Audit logging per user
     * - Easy to add/remove users
     * 
    try {
      const authToken = getAuthToken(); // From login session (JWT, cookie, etc.)
      
      const response = await fetch('/api/auth/mqtt-credentials', {
        method: 'GET',
        headers: {
          'Authorization': `Bearer ${authToken}`,
          'Content-Type': 'application/json'
        }
      });
      
      if (!response.ok) {
        throw new Error('Failed to fetch MQTT credentials');
      }
      
      const data = await response.json();
      // Expected response: { mqttUser, mqttPass, topicPrefix }
      
      // Store topic prefix for use in MQTT topics
      window.APP_CONFIG.TOPIC_PREFIX = data.topicPrefix; // e.g., "users/john123"
      
      return {
        user: data.mqttUser,      // e.g., "mqtt_user_john123"
        pass: data.mqttPass       // e.g., auto-generated secure password
      };
    } catch (error) {
      console.error('[Auth] Failed to get MQTT credentials:', error);
      throw new Error('Authentication failed. Please login again.');
    }
    */
  }
  
  // ==================== Application State ====================
  // UI state
  let pumpState = "UNKNOWN";   // "ON" | "OFF" | "UNKNOWN"
  let valveMode = "UNKNOWN";   // "1" | "2" | "UNKNOWN"
  
  // Timer state
  let timerState = {
    active: false,
    mode: 1, // 1 = Cascada, 2 = Eyectores
    duration: 3600, // seconds
    remaining: 0,
    interval: null
  };

  // ==================== DOM Elements Cache ====================
  // Cached DOM elements
  let elements = {};

  // ==================== Initialization ====================
  
  /**
   * Initialize the entire application
   * 
   * Sequence:
   * 1. Validate APP_CONFIG
   * 2. Cache DOM elements
   * 3. Initialize modules (Log, Programas)
   * 4. Setup MQTT event callbacks
   * 5. Wire UI event listeners
   * 6. Load stored credentials
   * 7. Auto-connect if credentials available
   */
  async function init() {
    // Validate APP_CONFIG
    if (!window.APP_CONFIG) {
      alert("APP_CONFIG no está definido. ¿Incluiste config.js?");
      throw new Error("APP_CONFIG no definido");
    }

    const { MQTT_WSS_URL, TOPIC_PUMP_CMD, TOPIC_PUMP_STATE, TOPIC_VALVE_CMD, TOPIC_VALVE_STATE } =
      window.APP_CONFIG;
    if (!MQTT_WSS_URL || !TOPIC_PUMP_CMD || !TOPIC_PUMP_STATE || !TOPIC_VALVE_CMD || !TOPIC_VALVE_STATE) {
      alert("APP_CONFIG incompleto: falta configuración de topics");
      throw new Error("APP_CONFIG incompleto");
    }

    // Cache all required DOM elements
    cacheElements();

    // Initialize logging module
    LogModule.init(
      elements.logBox,
      elements.btnLogToggle,
      elements.btnLogClear,
      elements.logContainer,
      elements.logToggleIcon,
      elements.logTimestamp
    );

    // Initialize Programas module
    if (window.ProgramasModule) {
      ProgramasModule.init();
    }

    // Setup MQTT event callbacks
    setupMQTTEvents();

    // Wire up UI event listeners
    wireUIEvents();

    // Start interval to update programas button
    setInterval(updateProgramasButton, 60000); // Update every minute
    updateProgramasButton(); // Initial update

    // Load stored credentials
    loadStoredCredentials();
    
    // Fetch weather temperature immediately and every 10 minutes
    fetchWeatherTemperature();
    setInterval(fetchWeatherTemperature, 10 * 60 * 1000); // Update every 10 minutes

    // Initialize UI state
    setPumpState("UNKNOWN");
    setValveMode("UNKNOWN");
    resetWiFiStatus();  // Reset WiFi status display
    disconnectUI();

    // Hide login card (single-user personal pool setup)
    // In future multi-user deployment, replace with login UI
    if (elements.loginCard) {
      elements.loginCard.style.display = 'none';
    }

    // Auto-connect to MQTT on page load
    try {
      LogModule.append('Conectando automáticamente...');
      const credentials = await getMQTTCredentials();
      connectMQTT(credentials.user, credentials.pass, MQTT_WSS_URL);
    } catch (error) {
      console.error('[Auth] Auto-connect failed:', error);
      LogModule.append('Error de autenticación');
      // Show login card if auto-connect fails (fallback to manual login)
      if (elements.loginCard) {
        elements.loginCard.style.display = 'block';
      }
    }

    // Cleanup on page unload
    window.addEventListener("beforeunload", () => {
      MQTTModule.disconnect();
    });
  }

  // ==================== DOM Element Caching ====================
  
  /**
   * Cache all required DOM elements at startup
   * Improves performance by avoiding repeated querySelector calls
   * Logs warning for any missing elements
   */
  function cacheElements() {
    const mapping = {
      "pump-label": "pumpLabel",
      "pump-icon": "pumpIcon",
      "pump-ring": "pumpRing",
      "pump-toggle-dot": "pumpToggleDot",
      "btn-valve-cascada": "btnValveCascada",
      "valve-cascada-dot": "valveCascadaDot",
      "btn-valve-eyectores": "btnValveEyectores",
      "valve-eyectores-dot": "valveEyectoresDot",
      "waterfall-icon": "waterfallIcon",
      "waterfall-label": "waterfallLabel",
      "waterjet-icon": "waterjetIcon",
      "waterjet-label": "waterjetLabel",
      "btn-timer": "btnTimer",
      "btn-programas": "btnProgramas",
      "conn-text": "connText",
      "conn-indicator": "connIndicator",
      "wifi-icon": "wifiIcon",
      "wifi-ssid": "wifiSsid",
      "temp-icon": "tempIcon",
      "temp-value": "tempValue",
      "temp-refresh-btn": "tempRefreshBtn",
      "weather-icon": "weatherIcon",
      "weather-temp": "weatherTemp",
      "btn-pump": "btnPump",
      "log-box": "logBox",
      "log-container": "logContainer",
      "log-toggle-icon": "logToggleIcon",
      "log-timestamp": "logTimestamp",
      "login-card": "loginCard",
      "mqtt-user": "userInput",
      "mqtt-pass": "passInput",
      "btn-connect": "btnConnect",
      "btn-log-toggle": "btnLogToggle",
      "btn-log-clear": "btnLogClear",
      "main-screen": "mainScreen",
      "timer-screen": "timerScreen",
      "btn-back": "btnBack",
      "header-title": "headerTitle",
      "timer-mode-1": "timerMode1",
      "timer-mode-2": "timerMode2",
      "timer-hours": "timerHours",
      "timer-minutes": "timerMinutes",
      "btn-timer-start": "btnTimerStart",
      "btn-timer-cancel": "btnTimerCancel",
      "btn-timer-stop": "btnTimerStop",
      "active-timer-card": "activeTimerCard",
      "timer-countdown": "timerCountdown",
      "timer-mode-display": "timerModeDisplay",
    };

    const missing = [];
    for (const [id, key] of Object.entries(mapping)) {
      const el = document.getElementById(id);
      if (!el) missing.push(id);
      elements[key] = el;
    }

    if (missing.length > 0) {
      console.warn("Missing DOM elements:", missing);
    }
  }

  // ==================== MQTT Event Callbacks ====================
  
  /**
   * Setup MQTT event callbacks
   * Registers callbacks for all MQTT events:
   * - State changes (pump, valve, timer, WiFi)
   * - Connection events (connected, disconnected)
   * - WiFi events and status updates
   */
  function setupMQTTEvents() {
    MQTTModule.onEvents(
      // onPumpStateChange callback
      (state) => {
        setPumpState(state);
      },
      // onValveStateChange callback
      (mode) => {
        setValveMode(mode);
      },
      // onConnected callback
      () => {
        connectUI();
      },
      // onDisconnected callback
      () => {
        disconnectUI();
      },
      // onWiFiEvent callback
      (event) => {
        LogModule.append(`[WiFi] ${event}`);
      },
      // onWiFiStateChange callback
      (wifiState) => {
        updateWiFiStatus(wifiState);
      },
      // onTimerStateChange callback
      (timerStateUpdate) => {
        handleTimerStateUpdate(timerStateUpdate);
      },
      // onTemperatureChange callback
      (temperature) => {
        updateTemperature(temperature);
      }
    );
  }

  // ==================== UI Event Listeners ====================
  
  /**
   * Wire up UI event listeners
   * Handles all user interactions:
   * - MQTT connection
   * - Pump control (with program conflict detection)
   * - Valve mode selection (with timer/program conflict handling)
   * - Timer management (start, stop, cancel)
   * - Screen navigation (back button)
   * - Program scheduling interface
   */
  function wireUIEvents() {
    // Shared handler for valve mode changes
    const requestValveMode = (requestedMode) => {
      // If clicking the active mode, switch to the opposite mode
      const newMode = (valveMode === requestedMode) ? (requestedMode === "1" ? "2" : "1") : requestedMode;
      const modeName = newMode === "1" ? "Cascada" : "Eyectores";

      // Timer conflict
      if (timerState.active) {
        const currentModeName = timerState.mode === 1 ? "Cascada" : "Eyectores";
        alert(`⚠️ Conflicto con Timer (${currentModeName}) - Pasando a control manual.`);
        LogModule.append("⚠️ Timer cancelado");
        stopTimer();
      }

      // Program conflict
      if (window.ProgramasModule) {
        const activeProgramName = ProgramasModule.getActiveProgramName();
        if (activeProgramName) {
          alert(`⚠️ Conflicto con Programa (${activeProgramName}) - Pasando a control manual.`);
          LogModule.append(`⚠️ Control manual - Programa "${activeProgramName}" en espera`);
          ProgramasModule.setManualOverride();
        }
      }

      LogModule.append(`Cambiando válvulas a modo ${newMode} (${modeName})...`);

      // Debounce both toggles
      if (elements.btnValveCascada) elements.btnValveCascada.disabled = true;
      if (elements.btnValveEyectores) elements.btnValveEyectores.disabled = true;
      setTimeout(() => {
        if (MQTTModule.isConnected()) {
          if (elements.btnValveCascada) elements.btnValveCascada.disabled = false;
          if (elements.btnValveEyectores) elements.btnValveEyectores.disabled = false;
        }
      }, BUTTON_DEBOUNCE_MS);

      MQTTModule.publish(
        newMode,
        window.APP_CONFIG.TOPIC_VALVE_CMD,
        (msg) => LogModule.append(msg)
      );
    };

    // Connect button (for manual reconnection, kept for testing/debugging)
    if (elements.btnConnect) {
      elements.btnConnect.addEventListener("click", async () => {
        try {
          const credentials = await getMQTTCredentials();
          connectMQTT(credentials.user, credentials.pass, window.APP_CONFIG.MQTT_WSS_URL);
        } catch (error) {
          LogModule.append(`Error de autenticación: ${error.message}`);
        }
      });
    }

    // Pump toggle button
    if (elements.btnPump) {
      elements.btnPump.addEventListener("click", () => {
        // Check if timer is active (must cancel timer to control pump manually)
        if (timerState.active) {
          const modeName = timerState.mode === 1 ? "Cascada" : "Eyectores";
          alert(`⚠️ Conflicto con Timer (${modeName}) - Pasando a control manual.`);
          LogModule.append("⚠️ Timer cancelado");
          stopTimer();
        }
        
        // Check if a program is active (conflict detection)
        if (window.ProgramasModule) {
          const activeProgramName = ProgramasModule.getActiveProgramName();
          if (activeProgramName) {
            alert(`⚠️ Conflicto con Programa (${activeProgramName}) - Pasando a control manual.`);
            LogModule.append(`⚠️ Control manual - Programa "${activeProgramName}" en espera`);
            ProgramasModule.setManualOverride();
          }
        }
        
        // Toggle based on current known state
        const newState = pumpState === "ON" ? "OFF" : "ON";
        const action = newState === "ON" ? "Encendiendo" : "Apagando";
        LogModule.append(`${action} bomba...`);
        
        // Temporarily disable button to prevent rapid clicking
        elements.btnPump.disabled = true;
        setTimeout(() => {
          if (MQTTModule.isConnected()) {
            elements.btnPump.disabled = false;
          }
        }, BUTTON_DEBOUNCE_MS);
        
        MQTTModule.publish(
          newState,
          window.APP_CONFIG.TOPIC_PUMP_CMD,
          (msg) => LogModule.append(msg)
        );
      });
    }

    // Valve mode toggles (mutually exclusive sliders)
    if (elements.btnValveCascada) {
      elements.btnValveCascada.addEventListener("click", () => requestValveMode("1"));
    }
    if (elements.btnValveEyectores) {
      elements.btnValveEyectores.addEventListener("click", () => requestValveMode("2"));
    }

    // Temperature refresh button
    if (elements.tempRefreshBtn) {
      elements.tempRefreshBtn.addEventListener("click", () => {
        LogModule.append("Refrescando temperatura...");
        MQTTModule.publish(
          "refresh",
          window.APP_CONFIG.TOPIC_TEMP_REFRESH,
          (msg) => LogModule.append(msg)
        );
        
        // Add visual feedback - spin the icon
        const icon = elements.tempRefreshBtn.querySelector(".material-icons-round");
        if (icon) {
          icon.style.transition = "transform 0.5s ease";
          icon.style.transform = "rotate(360deg)";
          setTimeout(() => {
            icon.style.transform = "rotate(0deg)";
          }, 500);
        }
      });
    }

    // Timer button
    if (elements.btnTimer) {
      elements.btnTimer.addEventListener("click", () => {
        showTimerScreen();
      });
    }

    // Programas button
    if (elements.btnProgramas) {
      elements.btnProgramas.addEventListener("click", () => {
        if (window.ProgramasModule) {
          ProgramasModule.showScreen();
        }
      });
    }

    // Timer screen navigation
    if (elements.btnBack) {
      elements.btnBack.addEventListener("click", () => {
        // Check which screen is active
        const timerActive = elements.timerScreen.classList.contains('slide-in');
        const programasActive = document.getElementById('programas-screen')?.classList.contains('translate-x-0');
        const createProgramActive = document.getElementById('create-program-screen')?.classList.contains('translate-x-0');
        
        if (createProgramActive && window.ProgramasModule) {
          // Hide create program screen, show programas list
          document.getElementById('create-program-screen').classList.remove('translate-x-0');
          document.getElementById('create-program-screen').classList.add('translate-x-full');
        } else if (programasActive && window.ProgramasModule) {
          ProgramasModule.hideScreen();
        } else if (timerActive) {
          hideTimerScreen();
        }
      });
    }

    if (elements.btnTimerCancel) {
      elements.btnTimerCancel.addEventListener("click", () => {
        hideTimerScreen();
      });
    }

    // Timer mode selection
    if (elements.timerMode1) {
      elements.timerMode1.addEventListener("click", () => {
        selectTimerMode(1);
      });
    }

    if (elements.timerMode2) {
      elements.timerMode2.addEventListener("click", () => {
        selectTimerMode(2);
      });
    }

    // Timer start button
    if (elements.btnTimerStart) {
      elements.btnTimerStart.addEventListener("click", () => {
        startTimer();
      });
    }

    // Timer stop button
    if (elements.btnTimerStop) {
      elements.btnTimerStop.addEventListener("click", () => {
        stopTimer();
      });
    }

    // Programas button (placeholder)
    if (elements.btnProgramas) {
      elements.btnProgramas.addEventListener("click", () => {
        LogModule.append("Programas: Funcionalidad próximamente");
      });
    }
  }

  // ==================== MQTT Connection Management ====================
  
  /**
   * Connect to MQTT broker
   * Initiates connection with username/password authentication
   * Subscribes to state topics for pump, valve, WiFi, and timer
   * 
   * @param {string} username - MQTT username
   * @param {string} password - MQTT password
   * @param {string} brokerUrl - WebSocket Secure URL (wss://)
   */
  function connectMQTT(username, password, brokerUrl) {
    MQTTModule.connect(
      brokerUrl,
      username,
      password,
      {
        pumpState: window.APP_CONFIG.TOPIC_PUMP_STATE,
        valveState: window.APP_CONFIG.TOPIC_VALVE_STATE,
        wifiState: window.APP_CONFIG.TOPIC_WIFI_STATE,
        timerState: window.APP_CONFIG.TOPIC_TIMER_STATE,
        tempState: window.APP_CONFIG.TOPIC_TEMP_STATE
      },
      window.APP_CONFIG.DEVICE_ID,
      (msg) => LogModule.append(msg)
    );
  }

  // ==================== State Management & UI Updates ====================
  
  /**
   * Update pump state display
   * Updates vertical toggle slider position based on state
   * 
   * @param {string} state - "ON" | "OFF" | "UNKNOWN"
   */
  function setPumpState(state) {
    pumpState = state;

    // Update horizontal toggle switch appearance
    if (elements.btnPump && elements.pumpToggleDot) {
      // Keep dot vertically centered
      elements.pumpToggleDot.style.top = '4px';
      elements.pumpToggleDot.style.bottom = 'auto';

      if (state === "OFF") {
        // Blue background, dot aligned right, icon at full opacity
        elements.btnPump.classList.remove('bg-slate-300');
        elements.btnPump.classList.add('bg-primary');
        elements.pumpToggleDot.style.left = 'calc(100% - 36px)';
        elements.pumpToggleDot.style.backgroundColor = '#e2e8f0';
        if (elements.pumpIcon) {
          elements.pumpIcon.style.opacity = '1';
        }
      } else if (state === "ON") {
        // Grey background, dot aligned left, icon greyed out
        elements.btnPump.classList.remove('bg-primary');
        elements.btnPump.classList.add('bg-slate-300');
        elements.pumpToggleDot.style.left = '4px';
        elements.pumpToggleDot.style.backgroundColor = 'white';
        if (elements.pumpIcon) {
          elements.pumpIcon.style.opacity = '0.3';
        }
      } else {
        // Unknown state - default to left, greyed out
        elements.btnPump.classList.remove('bg-primary');
        elements.btnPump.classList.add('bg-slate-300');
        elements.pumpToggleDot.style.left = '4px';
        elements.pumpToggleDot.style.backgroundColor = 'white';
        if (elements.pumpIcon) {
          elements.pumpIcon.style.opacity = '0.3';
        }
      }
    }

    updateButtonStates();
  }

  /**
   * Update valve mode display
   * Changes vertical toggle slider position to highlight active mode
   * Top = Mode 1 (Cascada), Bottom = Mode 2 (Eyectores)
   * 
   * @param {string} mode - "1" | "2" | "UNKNOWN"
   */
  function setValveMode(mode) {
    valveMode = mode;

    // Update horizontal sliders (mutually exclusive)
    const setSlider = (btn, dot, active) => {
      if (!btn || !dot) return;
      btn.classList.toggle('bg-primary', active);
      btn.classList.toggle('bg-slate-300', !active);
      dot.style.top = '4px';
      dot.style.left = active ? 'calc(100% - 36px)' : '4px';
      dot.style.backgroundColor = active ? '#e2e8f0' : 'white';
    };

    if (mode === "1") {
      setSlider(elements.btnValveCascada, elements.valveCascadaDot, true);
      setSlider(elements.btnValveEyectores, elements.valveEyectoresDot, false);
      if (elements.waterfallIcon) elements.waterfallIcon.style.opacity = '1';
      if (elements.waterfallLabel) elements.waterfallLabel.style.opacity = '1';
      if (elements.waterjetIcon) elements.waterjetIcon.style.opacity = '0.3';
      if (elements.waterjetLabel) elements.waterjetLabel.style.opacity = '0.3';
    } else if (mode === "2") {
      setSlider(elements.btnValveCascada, elements.valveCascadaDot, false);
      setSlider(elements.btnValveEyectores, elements.valveEyectoresDot, true);
      if (elements.waterjetIcon) elements.waterjetIcon.style.opacity = '1';
      if (elements.waterjetLabel) elements.waterjetLabel.style.opacity = '1';
      if (elements.waterfallIcon) elements.waterfallIcon.style.opacity = '0.3';
      if (elements.waterfallLabel) elements.waterfallLabel.style.opacity = '0.3';
    } else {
      setSlider(elements.btnValveCascada, elements.valveCascadaDot, false);
      setSlider(elements.btnValveEyectores, elements.valveEyectoresDot, false);
      if (elements.waterfallIcon) elements.waterfallIcon.style.opacity = '0.3';
      if (elements.waterfallLabel) elements.waterfallLabel.style.opacity = '0.3';
      if (elements.waterjetIcon) elements.waterjetIcon.style.opacity = '0.3';
      if (elements.waterjetLabel) elements.waterjetLabel.style.opacity = '0.3';
    }

    updateButtonStates();
  }

  /**
   * Update button enabled/disabled states based on connection
   * Disables all control buttons when MQTT is disconnected
   */
  function updateButtonStates() {
    const connected = MQTTModule.isConnected();

    if (elements.btnPump) {
      elements.btnPump.disabled = !connected;
    }
    if (elements.btnValveCascada) elements.btnValveCascada.disabled = !connected;
    if (elements.btnValveEyectores) elements.btnValveEyectores.disabled = !connected;
  }

  // ==================== Connection UI Feedback ====================
  
  /**
   * Update UI for connected state
   * Shows green animated indicator, hides login card, enables buttons
   */
  function connectUI() {
    if (elements.connText) elements.connText.textContent = "Conectado";
    
    // Update connection indicator with animated ping (green)
    if (elements.connIndicator) {
      elements.connIndicator.innerHTML = `
        <span class="animate-ping absolute inline-flex h-full w-full rounded-full bg-green-400 opacity-75"></span>
        <span class="relative inline-flex rounded-full h-2 w-2 bg-green-600"></span>
      `;
    }
    
    updateButtonStates();
    if (elements.loginCard) elements.loginCard.style.display = "none";
    LogModule.append("✓ Conectado al broker MQTT");
  }

  /**
   * Update UI for disconnected state
   * Shows red static indicator, displays login card, disables buttons
   */
  function disconnectUI() {
    if (elements.connText) elements.connText.textContent = "Desconectado";
    
    // Update connection indicator to red
    if (elements.connIndicator) {
      elements.connIndicator.innerHTML = `
        <span class="relative inline-flex rounded-full h-2 w-2 bg-red-600"></span>
      `;
    }
    
    updateButtonStates();
    if (elements.loginCard) elements.loginCard.style.display = "";
  }

  /**
   * Update WiFi status display
   * Shows WiFi icon with color-coded signal strength:
   * - Green: Excellent (>= -50 dBm)
   * - Blue: Good (>= -60 dBm)
   * - Yellow: Fair (>= -70 dBm)
   * - Orange: Weak (< -70 dBm)
   * - Red: Disconnected
   * 
   * @param {Object} wifiState - {status, ssid, ip, rssi, quality}
   */
  function updateWiFiStatus(wifiState) {
    if (!wifiState || wifiState.status !== "connected") {
      // Disconnected state - red
      if (elements.wifiIcon) elements.wifiIcon.textContent = "wifi_off";
      if (elements.wifiIcon) elements.wifiIcon.className = "material-icons-round text-red-600 text-lg";
      if (elements.wifiSsid) elements.wifiSsid.textContent = "Sin WiFi";
      
      // Clear temperature display when device is offline
      if (elements.tempValue) elements.tempValue.textContent = "--";
      return;
    }

    // Connected state
    const { ssid, ip, rssi, quality } = wifiState;
    
    // Update icon based on signal quality
    let icon = "wifi";
    let iconColor = "text-slate-400";
    
    if (quality === "excellent") {
      icon = "wifi";
      iconColor = "text-green-600";
    } else if (quality === "good") {
      icon = "wifi";
      iconColor = "text-blue-400";
    } else if (quality === "fair") {
      icon = "network_wifi_3_bar";
      iconColor = "text-yellow-500";
    } else {
      icon = "network_wifi_1_bar";
      iconColor = "text-orange-600";
    }
    
    if (elements.wifiIcon) {
      elements.wifiIcon.textContent = icon;
      elements.wifiIcon.className = `material-icons-round ${iconColor} text-lg`;
    }
    
    if (elements.wifiSsid) elements.wifiSsid.textContent = ssid || "WiFi";
  }

  /**
   * Reset WiFi status to disconnected state
   * Used on page initialization to clear any cached WiFi data
   */
  function resetWiFiStatus() {
    if (elements.wifiIcon) {
      elements.wifiIcon.textContent = "wifi_off";
      elements.wifiIcon.className = "material-icons-round text-slate-400 text-lg";
    }
    if (elements.wifiSsid) elements.wifiSsid.textContent = "Sin WiFi";
  }

  /**
   * Update temperature display
   * Shows pool water temperature in the circular display
   * 
   * @param {number|string} temperature - Temperature in Celsius
   */
  function updateTemperature(temperature) {
    const temp = parseFloat(temperature);
    
    if (isNaN(temp)) {
      if (elements.tempValue) elements.tempValue.textContent = "--";
      return;
    }
    
    // Update value display
    if (elements.tempValue) elements.tempValue.textContent = `${temp.toFixed(1)}°C`;
  }

  /**
   * Fetch and update weather temperature for Buenos Aires
   * Uses Open-Meteo API (free, no API key required)
   * Updates every 10 minutes
   */
  function fetchWeatherTemperature() {
    // Buenos Aires coordinates
    const latitude = -34.6037;
    const longitude = -58.3816;
    const url = `https://api.open-meteo.com/v1/forecast?latitude=${latitude}&longitude=${longitude}&current=temperature_2m,weather_code&timezone=auto`;
    
    fetch(url)
      .then(response => response.json())
      .then(data => {
        const temp = data.current.temperature_2m;
        const weatherCode = data.current.weather_code;
        
        if (elements.weatherTemp) {
          elements.weatherTemp.textContent = `${temp.toFixed(1)}°C`;
        }
        
        // Update icon based on weather code
        // 0 = clear, 1-3 = partly cloudy, 45-48 = fog, 51-67 = rain, 71-77 = snow, 80-99 = rain showers/thunderstorms
        let icon = 'wb_sunny';
        let color = 'text-amber-500';
        
        if (weatherCode === 0) {
          icon = 'wb_sunny';
          color = 'text-amber-500';
        } else if (weatherCode >= 1 && weatherCode <= 3) {
          icon = 'partly_cloudy_day';
          color = 'text-slate-500';
        } else if (weatherCode >= 45 && weatherCode <= 48) {
          icon = 'foggy';
          color = 'text-slate-400';
        } else if ((weatherCode >= 51 && weatherCode <= 67) || (weatherCode >= 80 && weatherCode <= 82)) {
          icon = 'rainy';
          color = 'text-blue-500';
        } else if (weatherCode >= 71 && weatherCode <= 77) {
          icon = 'ac_unit';
          color = 'text-cyan-400';
        } else if (weatherCode >= 95) {
          icon = 'thunderstorm';
          color = 'text-purple-600';
        }
        
        // Icon is fixed in HTML circular design, no need to update
      })
      .catch(error => {
        console.error('[WEATHER] Failed to fetch:', error);
        if (elements.weatherTemp) elements.weatherTemp.textContent = 'Error';
      });
  }

  // ==================== Credential Persistence ====================
  
  /**
   * Load credentials from localStorage
   * Auto-fills username and password fields if previously saved
   */
  function loadStoredCredentials() {
    if (elements.userInput) elements.userInput.value = localStorage.getItem(STORAGE_KEY_USER) || "";
    if (elements.passInput) elements.passInput.value = localStorage.getItem(STORAGE_KEY_PASS) || "";
  }

  /**
   * Save credentials to localStorage for future sessions
   * 
   * @param {string} user - MQTT username
   * @param {string} pass - MQTT password
   */
  function saveStoredCredentials(user, pass) {
    localStorage.setItem(STORAGE_KEY_USER, user);
    localStorage.setItem(STORAGE_KEY_PASS, pass);
  }

  // ==================== Screen Navigation ====================

  
  /**
   * Show timer screen with slide animation
   * Resets form to default values and validates MQTT connection
   */
  function showTimerScreen() {
    if (!MQTTModule.isConnected()) {
      LogModule.append("Conectá MQTT primero");
      return;
    }

    // Reset timer form
    selectTimerMode(timerState.mode);
    if (elements.timerHours) elements.timerHours.value = 1;
    if (elements.timerMinutes) elements.timerMinutes.value = 0;

    // Animate screen transition
    elements.mainScreen.classList.add('slide-left');
    elements.timerScreen.classList.add('slide-in');
    elements.btnBack.classList.remove('opacity-0', 'pointer-events-none');
    elements.headerTitle.textContent = 'Timer';
  }

  /**
   * Hide timer screen and return to main with reverse animation
   */
  function hideTimerScreen() {
    elements.mainScreen.classList.remove('slide-left');
    elements.timerScreen.classList.remove('slide-in');
    elements.btnBack.classList.add('opacity-0', 'pointer-events-none');
    elements.headerTitle.textContent = 'Smart Pool';
  }

  // ==================== Timer Management ====================
  
  /**
   * Select timer mode (1 = Cascada, 2 = Eyectores)
   * Updates button visual state
   * 
   * @param {number} mode - 1 (Cascada) or 2 (Eyectores)
   */
  function selectTimerMode(mode) {
    timerState.mode = mode;
    
    // Update UI
    if (mode === 1) {
      elements.timerMode1.classList.add('selected');
      elements.timerMode2.classList.remove('selected');
    } else {
      elements.timerMode1.classList.remove('selected');
      elements.timerMode2.classList.add('selected');
    }
  }

  /**
   * Start timer with selected settings
   * 
   * Sequence:
   * 1. Validate duration > 0
   * 2. Set valve mode
   * 3. Turn on pump (after delay)
   * 4. Start countdown interval
   * 5. Return to main screen
   */
  function startTimer() {
    const hours = parseInt(elements.timerHours.value) || 0;
    const minutes = parseInt(elements.timerMinutes.value) || 0;
    const totalSeconds = (hours * 3600) + (minutes * 60);

    if (totalSeconds === 0) {
      LogModule.append("⚠️ Configurá una duración válida");
      return;
    }

    // Set timer state
    timerState.active = true;
    timerState.duration = totalSeconds;
    timerState.remaining = totalSeconds;

    // Turn on pump
    LogModule.append(`🕐 Timer iniciado: ${hours}h ${minutes}m (Modo ${timerState.mode})`);
    
    // Set valve mode first
    MQTTModule.publish(
      timerState.mode.toString(),
      window.APP_CONFIG.TOPIC_VALVE_CMD,
      (msg) => LogModule.append(msg)
    );

    // Then turn on pump
    setTimeout(() => {
      MQTTModule.publish(
        "ON",
        window.APP_CONFIG.TOPIC_PUMP_CMD,
        (msg) => LogModule.append(msg)
      );
    }, VALVE_SWITCH_DELAY_MS);

    // Show active timer display
    elements.activeTimerCard.classList.remove('hidden');
    updateTimerDisplay();
    updateTimerButton();

    // Start countdown
    if (timerState.interval) clearInterval(timerState.interval);
    timerState.interval = setInterval(() => {
      timerState.remaining--;
      updateTimerDisplay();
      updateTimerButton();

      if (timerState.remaining <= 0) {
        stopTimer(true);
      }
    }, TIMER_UPDATE_INTERVAL);

    // Return to main screen after delay
    setTimeout(() => {
      hideTimerScreen();
    }, SCREEN_TRANSITION_DELAY);
  }

  /**
   * Stop active timer
   * Clears countdown interval, turns off pump, resets state
   * 
   * @param {boolean} autoStop - true if timer expired naturally, false if manually stopped
   */
  function stopTimer(autoStop = false) {
    if (!timerState.active) return;

    // Clear interval
    if (timerState.interval) {
      clearInterval(timerState.interval);
      timerState.interval = null;
    }

    // Turn off pump
    if (autoStop) {
      LogModule.append("⏰ Timer finalizado - Apagando bomba");
    } else {
      LogModule.append("🛑 Timer detenido manualmente");
    }

    MQTTModule.publish(
      "OFF",
      window.APP_CONFIG.TOPIC_PUMP_CMD,
      (msg) => LogModule.append(msg)
    );

    // Send command to ESP32 to stop the timer (duration: 0 stops the timer)
    const stopCommand = JSON.stringify({ mode: timerState.mode || 1, duration: 0 });
    MQTTModule.publish(
      stopCommand,
      window.APP_CONFIG.TOPIC_TIMER_CMD,
      () => {} // Silent - no log needed
    );

    // Reset state
    timerState.active = false;
    timerState.remaining = 0;

    // Hide timer display
    elements.activeTimerCard.classList.add('hidden');
    
    // Reset timer button text
    updateTimerButton();
  }

  // ==================== Display Updates ====================
  
  /**
   * Update timer button text with countdown
   * Shows HH:MM:SS when active, "Timer" when inactive
   */
  function updateTimerButton() {
    if (!elements.btnTimer) return;
    
    const timerText = elements.btnTimer.querySelector('span:last-child');
    if (!timerText) return;
    
    if (timerState.active && timerState.remaining > 0) {
      const hours = Math.floor(timerState.remaining / 3600);
      const minutes = Math.floor((timerState.remaining % 3600) / 60);
      const seconds = timerState.remaining % 60;
      
      const timeStr = `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
      timerText.textContent = timeStr;
    } else {
      timerText.textContent = 'Timer';
    }
  }

  /**
   * Update Programas button text with active program name
   * Shows program name when active, "Programas" when no program running
   * Adds visual ring highlight when program is active
   */
  function updateProgramasButton() {
    if (!window.ProgramasModule || !elements.btnProgramas) return;
    
    const activeProgramName = ProgramasModule.getActiveProgramName();
    const programasText = elements.btnProgramas.querySelector('span:last-child');
    
    if (activeProgramName) {
      // Truncate name if too long
      const displayName = activeProgramName.length > MAX_PROGRAM_NAME_LENGTH 
        ? activeProgramName.substring(0, MAX_PROGRAM_NAME_LENGTH) + '...' 
        : activeProgramName;
      programasText.textContent = displayName;
      
      // Highlight button to indicate active program
      if (!elements.btnProgramas.classList.contains('ring-2')) {
        elements.btnProgramas.classList.add('ring-2', 'ring-primary', 'ring-offset-2');
      }
    } else {
      programasText.textContent = 'Programas';
      elements.btnProgramas.classList.remove('ring-2', 'ring-primary', 'ring-offset-2');
    }
  }

  /**
   * Update timer countdown display in active timer card
   * Formats remaining time as HH:MM:SS and shows mode name
   */
  function updateTimerDisplay() {
    const hours = Math.floor(timerState.remaining / 3600);
    const minutes = Math.floor((timerState.remaining % 3600) / 60);
    const seconds = timerState.remaining % 60;

    const timeStr = `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
    elements.timerCountdown.textContent = timeStr;

    const modeName = timerState.mode === 1 ? 'Cascada' : 'Eyectores';
    elements.timerModeDisplay.textContent = `Modo: ${modeName}`;
  }

  /**
   * Handle timer state updates from MQTT
   * Syncs local timer with remote ESP32 timer state
   * Starts/stops local countdown to match remote state
   * 
   * @param {Object} stateUpdate - {active, mode, remaining, duration}
   */
  function handleTimerStateUpdate(stateUpdate) {
    if (stateUpdate.active) {
      // Sync local timer with remote state
      timerState.active = true;
      timerState.mode = stateUpdate.mode;
      timerState.remaining = stateUpdate.remaining;
      timerState.duration = stateUpdate.duration || timerState.remaining;

      // Show active timer card
      elements.activeTimerCard.classList.remove('hidden');
      updateTimerDisplay();

      // Start local countdown if not already running
      if (!timerState.interval) {
        timerState.interval = setInterval(() => {
          if (timerState.remaining > 0) {
            timerState.remaining--;
            updateTimerDisplay();
          }
        }, TIMER_UPDATE_INTERVAL);
      }
    } else {
      // Timer stopped remotely
      if (timerState.interval) {
        clearInterval(timerState.interval);
        timerState.interval = null;
      }
      timerState.active = false;
      timerState.remaining = 0;
      elements.activeTimerCard.classList.add('hidden');
    }
  }

  // ==================== Public API ====================
  // Minimal public interface - only init is exposed
  return {
    init,  // Initialize application (called on DOMContentLoaded)
  };
})();

// ==================== Application Bootstrap ====================
// Initialize app when DOM is ready
document.addEventListener("DOMContentLoaded", () => {
  AppModule.init().catch((err) => {
    console.error("App initialization failed:", err);
  });
});
