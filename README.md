# 🌿 Smart Green House Monitor

<div align="center">

![Smart Green House Monitor Banner](https://img.shields.io/badge/Smart%20Green%20House-IoT%20Monitor-16a34a?style=for-the-badge&logo=leaflet&logoColor=white)

[![ESP32](https://img.shields.io/badge/ESP32-Dev%20Module-blue?style=flat-square&logo=espressif&logoColor=white)](https://www.espressif.com/)
[![Node.js](https://img.shields.io/badge/Node.js-Express-339933?style=flat-square&logo=node.js&logoColor=white)](https://nodejs.org/)
[![Socket.IO](https://img.shields.io/badge/Realtime-Socket.IO-black?style=flat-square&logo=socket.io&logoColor=white)](https://socket.io/)


> An ESP32-based IoT system for environmental sensing, automated control, and remote door access —
> with a live web dashboard.

[Features](#features) • [Hardware](#hardware-components) • [Pin Mapping](#pin-mapping) • [Architecture](#system-architecture) • [Setup](#getting-started) • [Usage](#usage)

</div>

---

The system runs entirely on an ESP32, reading temperature, humidity, air quality, occupancy, and door position — then either reacts to those readings automatically or hands full manual control to a web dashboard. A servo-actuated door with an IR sensor adds physical access control, complete with an open-too-long safety alarm.

---

## Table of Contents

- [Features](#features)
- [Hardware Components](#hardware-components)
- [Pin Mapping](#pin-mapping)
- [System Architecture](#system-architecture)
- [Repository Structure](#repository-structure)
- [Getting Started](#getting-started)
- [Usage](#usage)
- [Notes](#notes)
- [Website Link](#website-link)


---

## Features

| | |
|---|---|
| 🌡️ **Live environmental monitoring** | Temperature, humidity, and air quality, updated every 5 seconds |
| 🧍 **Occupancy detection** | PIR motion sensor |
| 🔀 **Two control modes** | Auto (sensor-driven) and Manual (dashboard-driven), switchable at any time |
| 🌀 **Fan, light & O2 pump control** | 5V DC devices switched through relay modules, each independently controllable |
| 🚪 **Servo + IR door control** | Opens from the dashboard, auto-reverses shut once someone physically closes it, with a 30-second open-door buzzer alarm |
| 🖥️ **On-device LCD** | Mirrors current state locally, independent of network connectivity |
| 🌐 **Real-time web dashboard** | Built on Socket.IO, reflects and controls the system from any browser |
| 📲 **Telegram alerts** | Pushed for key events (high temperature/humidity, pump activation, door left open) |

---

## Hardware Components

| Component | Role |
|---|---|
| ESP32 Dev Module | Main controller — WiFi, control loop, HTTP client |
| DHT22 | Temperature & humidity sensor |
| MQ-135 | Air quality sensor (used as a CO2/stale-air proxy) |
| PIR motion sensor | Room occupancy detection |
| HW-201 IR sensor | Digital door-position sensor (in-range / out-of-range only) |
| SG90 servo motor | Physically opens/closes the door |
| Sunstar 2004A 20×4 I2C LCD | Local status display |
| 2-channel 5V relay module | Switches Fan and O2 Pump |
| 1-channel 5V relay module | Switches Light |
| Active piezo buzzer | Direct GPIO drive (not relay-switched) |

---

## Pin Mapping

| Component | Pin | Notes |
|---|---|---|
| DHT22 (Data) | GPIO 4 | 10 kΩ pull-up to 3.3V |
| MQ-135 (AO) | GPIO 36 | Voltage divider 10k + 10k |
| PIR (OUT) | GPIO 32 | 3.3V TTL |
| LCD (SDA / SCL) | GPIO 21 / 22 | I2C, 5V backpack |
| Fan | GPIO 26 | 2-channel relay, channel 1 |
| O2 Pump | GPIO 27 | 2-channel relay, channel 2 |
| Light | GPIO 19 | 1-channel relay module |
| Buzzer (+) | GPIO 25 | Direct piezo |
| HW-201 IR (OUT) | GPIO 34 | Input-only pin, digital HIGH/LOW only |
| Servo (signal) | GPIO 13 | PWM; separate 5V supply, common GND |

> **Power note:** All relay-driven devices (Fan, Light, O2 Pump) are 5V DC loads — none are wired to AC mains. The servo runs off its own dedicated 5V supply (to avoid brownouts from current spikes), sharing a common ground bus with the ESP32 and the main 5V adapter.

---

## System Architecture

```
Sensors (DHT22, MQ-135, PIR, HW-201)
        │
        ▼
      ESP32  ──── WiFi ────▶  Node.js server  ──── Socket.IO ────▶  Web dashboard
        │                         │                                     │
        ▼                         └──── Telegram alerts                 │
Relays (Fan, Light, Pump)                                                │
Servo (door) + Buzzer                                                    │
LCD (local display)                                                      │
        ▲                                                                │
        └────────────────── manual commands ◀─────────────────────────────┘
```

- The ESP32 POSTs a JSON payload (sensor readings + device/door state) to the server every 5 seconds
- The server stores the reading, broadcasts it to all connected dashboards in real time, and returns the current mode plus any pending manual commands
- In **Auto mode**, the ESP32 decides device states itself from sensor readings
- In **Manual mode**, the ESP32 instead applies whatever the dashboard last set
- The dashboard has no knowledge of physical GPIOs or relay modules — it only ever refers to devices by name, so hardware rewiring never requires a website change

---

## Repository Structure

```
├── SmartRoomMonitor.ino   # ESP32 firmware
├── server.js              # Node.js/Express + Socket.IO backend
├── public/
│   └── index.html         # Web dashboard (HTML/CSS/JS)
├── package.json
└── README.md
```

---

## Getting Started

### 1. Firmware (ESP32)

1. Open `SmartRoomMonitor.ino` in the Arduino IDE.
2. Install the required libraries via the Library Manager:
   - `DHT sensor library` (Adafruit)
   - `Adafruit Unified Sensor`
   - `LiquidCrystal I2C` (Frank de Brabander)
   - `ESP32Servo`
   - `ArduinoJson` (v6.x)
3. Update these lines with your own network and server details:
   ```cpp
   const char* WIFI_SSID  = "your-wifi-name";
   const char* WIFI_PASS  = "your-wifi-password";
   const char* SERVER_URL = "https://your-deployment.onrender.com/api/data";
   ```
4. Select **ESP32 Dev Module** as the board, choose the correct COM port, and upload.
5. Open the Serial Monitor at **115200 baud** to confirm WiFi connection and see live logs.

### 2. Server

```bash
npm install
node server.js
```

Set any required environment variables (e.g. Telegram bot token/chat ID) in a `.env` file — see `dotenv` usage in `server.js`. Deploy to a host like Render, and point the firmware's `SERVER_URL` at the deployed address.

### 3. Dashboard

Once the server is running, open its URL in a browser. The dashboard connects over Socket.IO automatically and starts reflecting live sensor data.

---

## Usage

| Control | Behavior |
|---|---|
| **Mode toggle** | Switch between Auto and Manual at the top of the dashboard |
| **Device cards** | Fan, Light, Buzzer, O2 Pump each show live on/off state (LED-style indicator) and can be toggled directly in Manual mode |
| **Door override** | "Open door" actuates the servo and arms auto-reverse-on-physical-close plus the 30-second alarm; "Close now" is a manual override at any time |
| **Sensor cards** | Live temperature, humidity, and air quality readings, plus occupancy and door status |

---

## Notes

- The MQ-135 is used as a CO2/stale-air proxy in place of a dedicated O2 sensor.
- The HW-201 only reports in-range/out-of-range, not distance — mounting position and the onboard potentiometer determine detection reliability.
- Relay modules used here are active-LOW; polarity is handled centrally in firmware via a single `RELAY_ACTIVE_LOW` flag.

---

## Website Link
- [LINK](#https://green-house-monitor.onrender.com/)
