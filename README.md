# HLK-LD2451 24GHz Radar Telemetry & Real-Time Web Dashboard (ESP32)

A dual-core PlatformIO firmware and real-time WebSocket dashboard for interfacing the **Hi-Link HLK-LD2451 24GHz FMCW Vehicle Radar Sensor** with an **ESP32 / ESP32-S3**.

---

## Key Features

- **Zero-Drop Dual-Core UART Parsing:** A dedicated FreeRTOS task on **Core 1** continuously reads `UART2` into a 4 KB buffer and parses incoming binary frames without blocking on network or Serial I/O.
- **Resilient Frame Parser:** Automatically resyncs on partial/corrupt headers, validates frame tails and payload lengths, handles zero-target frames, and filters out command-reply frames (`FD FC FB FA`).
- **Real-Time Web Dashboard (20 Hz):**
  - **2D Radar Sector View:** Live polar display (`±30°` grid with `±20°` sensor field-of-view limits) tracking distance, angle, speed, and direction.
  - **60-Second Rolling Charts:** Live distance and speed telemetry for the nearest target (includes built-in offline canvas rendering when Chart.js CDN is unavailable in AP/hotspot mode).
  - **Live Target Table & Diagnostics:** Displays active targets, approaching alarm badge, raw hex frame inspector, and parser health stats (parsed vs. dropped frames).
- **Flexible Wi-Fi Modes:** Supports both Router (Station) mode with mDNS (`http://ld2451.local`) and automatic fallback to standalone Hotspot (AP) mode.
- **Interactive USB Serial Monitor:** Formatted live target table, raw hex dump, and runtime keyboard commands.

---

## Hardware Wiring

> **Power Note:** The HLK-LD2451 requires **5V DC (≥ 300 mA)**. Do **not** power it from the ESP32 `3V3` pin. Its UART pins operate at **3.3V logic** and connect directly to the ESP32.

| HLK-LD2451 Pin | ESP32 Pin | Notes |
| :--- | :--- | :--- |
| **VIN (5V)** | `5V` / `VIN` | Requires ≥ 300 mA supply |
| **GND** | `GND` | Common ground |
| **TX** | `GPIO 16` (RX2) | 115200 baud, 3.3V logic |
| **RX** | `GPIO 17` (TX2) | 115200 baud, 3.3V logic |

---

## Getting Started

### 1. Configure Wi-Fi
Open `src/main.cpp` and set your Wi-Fi credentials and mode (`WIFI_USE_STATION`):
- **Station Mode (`true`):** Connects to your Wi-Fi router. If connection fails within 15 seconds, it automatically falls back to Hotspot mode. Access the dashboard at:
  ```text
  http://ld2451.local
  ```
- **Hotspot / AP Mode (`false`):** Broadcasts its own Wi-Fi network so you can connect directly from your laptop or phone.

### 2. Build & Flash (PlatformIO)

**For standard ESP32:**
```bash
pio run -t upload
```

**For ESP32-S3:**
```bash
pio run -e esp32s3 -t upload
```

---

## Serial Monitor Commands

Open the Serial Monitor at **`115200` baud** and send any of the following single-character commands:

| Command | Action |
| :---: | :--- |
| `r` | Toggle raw hex frame dump |
| `p` | Toggle formatted target table output |
| `s` | Show parser & WebSocket health statistics |
| `?` | Display help menu |

---

## Troubleshooting & Calibration

- **Mirrored Left/Right Targets:** If targets appear flipped horizontally on the radar sector view, set `ANGLE_SIGN = -1` in the dashboard configuration.
- **Decoding Verification:** The parser follows the standard `F4 F3 F2 F1` ... `F8 F7 F6 F5` target reporting protocol. Use the `r` command in the Serial Monitor to inspect raw incoming hex frames if needed.
