/******************************************************************************
 *  HLK-LD2451 24 GHz Vehicle Radar  ->  ESP32 Real-Time Web Dashboard
 *  --------------------------------------------------------------------------
 *  Target    : ESP32 / ESP32-S3 (Arduino core 2.x or 3.x)
 *  Radar link: UART2  RX = GPIO16  (connect to LD2451 TX)
 *                     TX = GPIO17  (connect to LD2451 RX)
 *                     115200 8N1 (module default)
 *  Debug     : USB Serial @ 115200
 *  Web UI    : http://<esp-ip>/   (or http://ld2451.local/ in STA mode)
 *              WebSocket stream on ws://<esp-ip>/ws
 *
 *  HARDWARE NOTES (from the HLK-LD2451 manual v1.0)
 *   - Supply the module with 5 V, >= 300 mA capable (avg. ~107 mA).
 *     Do NOT power it from the ESP32 3V3 pin.
 *   - Module IO is 3.3 V -> wire directly to ESP32 GPIOs, no level shifter.
 *   - Common GND between ESP32 and radar is mandatory.
 *   - Rated detection: up to 100 m, ±20° (dashboard grid shows ±30°).
 *
 *  ARCHITECTURE (why UART bytes are never lost to Wi-Fi activity)
 *   ┌──────────────── core 1 ────────────────┐   ┌──────── core 0 ────────┐
 *   │ radarRxTask (prio 10)                  │   │ Wi-Fi + AsyncTCP       │
 *   │  UART2 4 KB ring buffer -> byte-level  │   │ (lwIP, WebSocket send) │
 *   │  state machine -> FreeRTOS queue       │   └────────────────────────┘
 *   │ loop() (prio 1)                        │
 *   │  drain queue -> Serial debug print     │
 *   │  -> throttled WebSocket JSON broadcast │
 *   └────────────────────────────────────────┘
 *   The parser task never prints and never touches the network, so a slow
 *   Serial monitor or a congested Wi-Fi link can only delay/skip *display*
 *   of frames (counted as "dropped"), never corrupt UART reception.
 *
 *  LIBRARIES  (see platformio.ini; Arduino IDE: install via Library Manager)
 *   - ESPAsyncWebServer  (ESP32Async)   >= 3.7
 *   - AsyncTCP           (ESP32Async)   >= 3.3
 *   - ArduinoJson        (bblanchon)    >= 7.0
 *
 *  ARDUINO IDE: rename this file to LD2451_Radar.ino, put it in a folder
 *  named LD2451_Radar together with dashboard_html.h.
 ******************************************************************************/

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "dashboard_html.h"

// ============================================================================
//  1. USER CONFIGURATION
// ============================================================================

// ---------------------------------------------------------------------------
//  Wi-Fi MODE SELECTION
// ---------------------------------------------------------------------------
//  WIFI_USE_STATION = 1  -> STATION (STA) mode: ESP32 joins your router.
//                           Set WIFI_STA_SSID / WIFI_STA_PASS below.
//                           Open http://ld2451.local/ or the IP printed on
//                           the Serial Monitor at boot.
//
//  WIFI_USE_STATION = 0  -> ACCESS POINT (AP) mode: ESP32 creates its own
//                           hotspot (WIFI_AP_SSID / WIFI_AP_PASS).
//                           Connect your phone/laptop to it and open
//                           http://192.168.4.1/
//
//  STA_FALLBACK_TO_AP = 1 -> In STA mode, if the router can't be joined
//                           within WIFI_STA_TIMEOUT_MS, the ESP32 starts the
//                           AP instead, so the dashboard is always reachable.
//
//  NOTE ON AP MODE: a client on the ESP32 hotspot usually has no internet,
//  so the Chart.js CDN cannot load. The dashboard detects this and switches
//  the two time-series charts to a built-in lightweight canvas plotter; the
//  radar sweep and table are pure canvas/JS and always work.
// ---------------------------------------------------------------------------
#define WIFI_USE_STATION      1
#define STA_FALLBACK_TO_AP    1

static const char* WIFI_STA_SSID = "YOUR_WIFI_SSID";
static const char* WIFI_STA_PASS = "YOUR_WIFI_PASSWORD";
static const uint32_t WIFI_STA_TIMEOUT_MS = 15000;

static const char* WIFI_AP_SSID  = "LD2451-Radar";
static const char* WIFI_AP_PASS  = "radar2451";     // >= 8 chars, or "" for open
static const uint8_t WIFI_AP_CHANNEL = 6;

static const char* MDNS_HOSTNAME = "ld2451";         // -> http://ld2451.local/

// ---------------------------------------------------------------------------
//  Radar UART
// ---------------------------------------------------------------------------
#define RADAR_UART_NUM        2
#define RADAR_RX_PIN          16      // ESP32 RX  <- LD2451 TX
#define RADAR_TX_PIN          17      // ESP32 TX  -> LD2451 RX
#define RADAR_BAUD            115200
#define RADAR_RX_BUFFER_SIZE  4096    // driver ring buffer (~350 ms @115200)

// ---------------------------------------------------------------------------
//  Behaviour
// ---------------------------------------------------------------------------
#define DEBUG_BAUD             115200
#define WS_MIN_INTERVAL_MS     50      // max 20 WebSocket pushes / s
#define STATUS_INTERVAL_MS     1000    // periodic status/heartbeat push
#define STALE_TIMEOUT_MS       2000    // no frame for this long -> clear UI
#define FRAME_QUEUE_DEPTH      16

// Runtime-toggleable from the Serial Monitor (send 'r', 'p', 's', '?')
static volatile bool gDebugRawHex = true;
static volatile bool gDebugPretty = true;

// ============================================================================
//  2. LD2451 PROTOCOL DEFINITIONS
// ============================================================================
//
//  Frame layout (all multi-byte fields little-endian):
//
//   +-------------+--------+-------+-------+----------------------+-------------+
//   | F4 F3 F2 F1 | LEN(2) | COUNT | ALARM | COUNT x 5-byte tgt   | F8 F7 F6 F5 |
//   +-------------+--------+-------+-------+----------------------+-------------+
//                  LEN = number of bytes between LEN and the tail
//
//   Target (5 bytes):
//     [0] angle    : degrees = raw - 0x80  (signed, 0 = boresight)
//     [1] distance : metres (uint8)
//     [2] direction: 1 = approaching, 0 = moving away
//     [3] speed    : km/h (uint8)
//     [4] SNR      : uint8
//
//   ALARM: 0x01 = approaching-target alarm active.
//   LEN == 0 is accepted as "no targets" (some firmware sends empty frames).
//   Command ACK frames (FD FC FB FA ... 04 03 02 01) are ignored automatically
//   because the parser only locks onto the data-frame header.
// ============================================================================

static const uint8_t LD_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t LD_TAIL[4]   = {0xF8, 0xF7, 0xF6, 0xF5};

#define LD_MAX_TARGETS     10
#define LD_TARGET_BYTES    5
#define LD_MAX_PAYLOAD     (2 + LD_MAX_TARGETS * LD_TARGET_BYTES)
#define LD_MAX_FRAME       (4 + 2 + LD_MAX_PAYLOAD + 4)

struct RadarTarget {
  int8_t   angleDeg;     // -128..+127, practically ±30
  uint8_t  distanceM;
  uint8_t  approaching;  // 1 = approaching, 0 = away
  uint8_t  speedKmh;
  uint8_t  snr;
};

struct RadarFrame {
  uint32_t    timestampMs;
  uint32_t    seq;
  uint8_t     targetCount;
  uint8_t     alarm;
  RadarTarget targets[LD_MAX_TARGETS];
  uint8_t     rawLen;
  uint8_t     raw[LD_MAX_FRAME];
};

struct ParserStats {
  volatile uint32_t bytesRx      = 0;
  volatile uint32_t framesOk     = 0;
  volatile uint32_t errLength    = 0;   // LEN out of range / inconsistent
  volatile uint32_t errTail      = 0;   // tail mismatch (corrupt / desync)
  volatile uint32_t queueDropped = 0;   // consumer too slow (display only)
};
static ParserStats gStats;

// ============================================================================
//  3. NON-BLOCKING BYTE-LEVEL STATE MACHINE PARSER
// ============================================================================
//  feed() consumes exactly one byte and returns immediately; it returns true
//  when a complete, validated frame has been decoded into `out`.
//  Resynchronisation: any unexpected byte drops back to header hunting. The
//  header bytes are all distinct, so a single-step restart is sufficient.
// ============================================================================
class LD2451Parser {
 public:
  bool feed(uint8_t b, RadarFrame& out) {
    switch (state_) {
      case State::Header:
        if (b == LD_HEADER[idx_]) {
          buf_[idx_++] = b;
          if (idx_ == 4) { rawLen_ = 4; state_ = State::LenLo; }
        } else {
          // Restart; the mismatching byte may itself be a new header start
          idx_ = 0;
          if (b == LD_HEADER[0]) { buf_[0] = b; idx_ = 1; }
        }
        break;

      case State::LenLo:
        buf_[rawLen_++] = b;
        len_ = b;
        state_ = State::LenHi;
        break;

      case State::LenHi:
        buf_[rawLen_++] = b;
        len_ |= (uint16_t)b << 8;
        if (len_ == 0) {
          idx_ = 0; state_ = State::Tail;               // empty frame
        } else if (len_ < 2 || len_ > LD_MAX_PAYLOAD) {
          gStats.errLength++; reset();                  // impossible length
        } else {
          idx_ = 0; state_ = State::Payload;
        }
        break;

      case State::Payload:
        buf_[rawLen_++] = b;
        if (++idx_ >= len_) { idx_ = 0; state_ = State::Tail; }
        break;

      case State::Tail:
        if (b == LD_TAIL[idx_]) {
          buf_[rawLen_++] = b;
          if (++idx_ == 4) {
            bool ok = decode(out);
            reset();
            return ok;
          }
        } else {
          gStats.errTail++;
          reset();
          if (b == LD_HEADER[0]) { buf_[0] = b; idx_ = 1; }
        }
        break;
    }
    return false;
  }

 private:
  enum class State : uint8_t { Header, LenLo, LenHi, Payload, Tail };

  void reset() { state_ = State::Header; idx_ = 0; len_ = 0; rawLen_ = 0; }

  bool decode(RadarFrame& out) {
    const uint8_t* p = &buf_[6];                        // start of payload
    out.timestampMs = millis();
    out.seq         = ++seq_;
    out.targetCount = 0;
    out.alarm       = 0;

    if (len_ >= 2) {
      uint8_t count = p[0];
      // Validate: declared count must fit inside LEN and our buffer
      if (count > LD_MAX_TARGETS ||
          (uint16_t)(2 + count * LD_TARGET_BYTES) > len_) {
        gStats.errLength++;
        return false;
      }
      out.targetCount = count;
      out.alarm       = p[1];
      const uint8_t* t = &p[2];
      for (uint8_t i = 0; i < count; i++, t += LD_TARGET_BYTES) {
        out.targets[i].angleDeg    = (int8_t)((int16_t)t[0] - 0x80);
        out.targets[i].distanceM   = t[1];
        out.targets[i].approaching = (t[2] == 0x01) ? 1 : 0;
        out.targets[i].speedKmh    = t[3];
        out.targets[i].snr         = t[4];
      }
    }
    out.rawLen = rawLen_;
    memcpy(out.raw, buf_, rawLen_);
    gStats.framesOk++;
    return true;
  }

  State    state_  = State::Header;
  uint8_t  idx_    = 0;
  uint16_t len_    = 0;
  uint8_t  rawLen_ = 0;
  uint32_t seq_    = 0;
  uint8_t  buf_[LD_MAX_FRAME];
};

// ============================================================================
//  4. GLOBALS
// ============================================================================
static HardwareSerial  RadarSerial(RADAR_UART_NUM);
static LD2451Parser    gParser;
static QueueHandle_t   gFrameQueue = nullptr;

static AsyncWebServer  gServer(80);
static AsyncWebSocket  gWs("/ws");

static RadarFrame      gLatest;              // owned by loop()
static bool            gLatestPending = false;
static uint32_t        gLastFrameMs   = 0;
static uint32_t        gLastWsTxMs    = 0;
static uint32_t        gLastStatusMs  = 0;
static bool            gStaleSent     = false;
static bool            gApMode        = false;

// ============================================================================
//  5. UART RECEIVE TASK (core 1, high priority, never blocks on I/O)
// ============================================================================
static void pushFrame(const RadarFrame& f) {
  if (xQueueSend(gFrameQueue, &f, 0) != pdTRUE) {
    // Queue full: discard the oldest so the UI always shows the freshest data
    RadarFrame discard;
    xQueueReceive(gFrameQueue, &discard, 0);
    xQueueSend(gFrameQueue, &f, 0);
    gStats.queueDropped++;
  }
}

static void radarRxTask(void*) {
  static RadarFrame frame;                    // static: keeps task stack small
  uint8_t chunk[128];
  for (;;) {
    int avail = RadarSerial.available();
    if (avail <= 0) {
      // 2 ms at 115200 baud = ~23 bytes, far below the 4 KB driver buffer
      vTaskDelay(pdMS_TO_TICKS(2));
      continue;
    }
    size_t n = RadarSerial.read(chunk, min(avail, (int)sizeof(chunk)));
    gStats.bytesRx += n;
    for (size_t i = 0; i < n; i++) {
      if (gParser.feed(chunk[i], frame)) pushFrame(frame);
    }
  }
}

// ============================================================================
//  6. SERIAL DEBUG OUTPUT
// ============================================================================
static void printHexFrame(const RadarFrame& f) {
  // Build the whole line first, then print once (fewer, larger writes)
  char line[16 + LD_MAX_FRAME * 3];
  int pos = snprintf(line, sizeof(line), "  RAW (%2u B): ", f.rawLen);
  for (uint8_t i = 0; i < f.rawLen && pos < (int)sizeof(line) - 4; i++) {
    pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", f.raw[i]);
  }
  Serial.println(line);
}

static void printPrettyFrame(const RadarFrame& f) {
  Serial.printf("[%10lu ms] Frame #%-6lu | Targets: %u | Alarm: %s\n",
                (unsigned long)f.timestampMs, (unsigned long)f.seq,
                f.targetCount,
                f.alarm == 0x01 ? "*** APPROACHING ***" : "none");
  if (f.targetCount == 0) return;
  Serial.println("   ID | Angle  | Dist   | Direction   | Speed     | SNR");
  Serial.println("  ----+--------+--------+-------------+-----------+-----");
  for (uint8_t i = 0; i < f.targetCount; i++) {
    const RadarTarget& t = f.targets[i];
    Serial.printf("   %2u | %+4d°  | %3u m  | %-11s | %3u km/h  | %3u\n",
                  i + 1, t.angleDeg, t.distanceM,
                  t.approaching ? "APPROACHING" : "MOVING AWAY",
                  t.speedKmh, t.snr);
  }
}

static void printStats() {
  Serial.printf(
      "\n--- STATS --- up %lus | bytes %lu | frames %lu | lenErr %lu | "
      "tailErr %lu | dropped %lu | heap %lu | WS clients %u | IP %s\n\n",
      (unsigned long)(millis() / 1000), (unsigned long)gStats.bytesRx,
      (unsigned long)gStats.framesOk, (unsigned long)gStats.errLength,
      (unsigned long)gStats.errTail, (unsigned long)gStats.queueDropped,
      (unsigned long)ESP.getFreeHeap(), (unsigned)gWs.count(),
      (gApMode ? WiFi.softAPIP() : WiFi.localIP()).toString().c_str());
}

static void printHelp() {
  Serial.println(F("\nSerial commands:  r = toggle raw hex   p = toggle pretty "
                   "print   s = stats   ? = help\n"));
}

static void handleSerialCommands() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'r': gDebugRawHex = !gDebugRawHex;
                Serial.printf("Raw hex: %s\n", gDebugRawHex ? "ON" : "OFF"); break;
      case 'p': gDebugPretty = !gDebugPretty;
                Serial.printf("Pretty: %s\n", gDebugPretty ? "ON" : "OFF"); break;
      case 's': printStats(); break;
      case '?': case 'h': printHelp(); break;
      default: break;
    }
  }
}

// ============================================================================
//  7. WEBSOCKET JSON BROADCAST
// ============================================================================
//  Frame message:
//   {"type":"frame","seq":123,"ts":45678,"alarm":1,
//    "targets":[{"id":1,"ang":-5,"dist":42,"dir":1,"spd":55,"snr":120}],
//    "raw":"F4 F3 ..."}
//  Status message (1 Hz):
//   {"type":"status","up":123,"rssi":-55,"heap":200000,"clients":1,
//    "frames":..,"lenErr":..,"tailErr":..,"dropped":..,"stale":false,"mode":"STA"}
// ============================================================================
static void wsBroadcastFrame(const RadarFrame& f) {
  if (gWs.count() == 0) return;

  JsonDocument doc;
  doc["type"]  = "frame";
  doc["seq"]   = f.seq;
  doc["ts"]    = f.timestampMs;
  doc["alarm"] = f.alarm;
  JsonArray arr = doc["targets"].to<JsonArray>();
  for (uint8_t i = 0; i < f.targetCount; i++) {
    const RadarTarget& t = f.targets[i];
    JsonObject o = arr.add<JsonObject>();
    o["id"]   = i + 1;
    o["ang"]  = t.angleDeg;
    o["dist"] = t.distanceM;
    o["dir"]  = t.approaching;
    o["spd"]  = t.speedKmh;
    o["snr"]  = t.snr;
  }
  char hex[LD_MAX_FRAME * 3 + 1];
  int pos = 0;
  for (uint8_t i = 0; i < f.rawLen; i++)
    pos += snprintf(hex + pos, sizeof(hex) - pos, i ? " %02X" : "%02X", f.raw[i]);
  hex[pos] = '\0';
  doc["raw"] = hex;   // copied into the document

  char out[1024];
  size_t n = serializeJson(doc, out, sizeof(out));
  if (n > 0) gWs.textAll(out, n);
}

static void wsBroadcastStatus(bool stale) {
  if (gWs.count() == 0) return;
  JsonDocument doc;
  doc["type"]    = "status";
  doc["up"]      = millis() / 1000;
  doc["rssi"]    = gApMode ? 0 : WiFi.RSSI();
  doc["heap"]    = ESP.getFreeHeap();
  doc["clients"] = gWs.count();
  doc["frames"]  = gStats.framesOk;
  doc["bytes"]   = gStats.bytesRx;
  doc["lenErr"]  = gStats.errLength;
  doc["tailErr"] = gStats.errTail;
  doc["dropped"] = gStats.queueDropped;
  doc["stale"]   = stale;
  doc["mode"]    = gApMode ? "AP" : "STA";
  char out[384];
  size_t n = serializeJson(doc, out, sizeof(out));
  if (n > 0) gWs.textAll(out, n);
}

static void onWsEvent(AsyncWebSocket* server, AsyncWebSocketClient* client,
                      AwsEventType type, void* arg, uint8_t* data, size_t len) {
  switch (type) {
    case WS_EVT_CONNECT:
      Serial.printf("[WS] client #%lu connected from %s\n",
                    (unsigned long)client->id(),
                    client->remoteIP().toString().c_str());
      gLastStatusMs = 0;                       // push status immediately
      break;
    case WS_EVT_DISCONNECT:
      Serial.printf("[WS] client #%lu disconnected\n",
                    (unsigned long)client->id());
      break;
    case WS_EVT_ERROR:
      Serial.printf("[WS] client #%lu error\n", (unsigned long)client->id());
      break;
    default:
      break;                                   // dashboard is receive-only
  }
}

// ============================================================================
//  8. WI-FI
// ============================================================================
static void startAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASS, WIFI_AP_CHANNEL);
  gApMode = true;
  Serial.printf("[WiFi] AP mode  SSID: \"%s\"  PASS: \"%s\"\n",
                WIFI_AP_SSID, WIFI_AP_PASS);
  Serial.printf("[WiFi] Dashboard: http://%s/\n",
                WiFi.softAPIP().toString().c_str());
}

static void startWiFi() {
#if WIFI_USE_STATION
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);                       // lower WebSocket latency
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(MDNS_HOSTNAME);
  WiFi.begin(WIFI_STA_SSID, WIFI_STA_PASS);
  Serial.printf("[WiFi] Connecting to \"%s\"", WIFI_STA_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_STA_TIMEOUT_MS) {
    delay(250);                               // radarRxTask keeps running
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    gApMode = false;
    Serial.printf("[WiFi] STA connected  IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
    Serial.printf("[WiFi] Dashboard: http://%s/  or  http://%s.local/\n",
                  WiFi.localIP().toString().c_str(), MDNS_HOSTNAME);
  } else {
  #if STA_FALLBACK_TO_AP
    Serial.println("[WiFi] STA failed -> falling back to Access Point");
    WiFi.disconnect(true);
    startAccessPoint();
  #else
    Serial.println("[WiFi] STA not connected yet; will keep retrying");
  #endif
  }
#else
  startAccessPoint();
#endif

  if (MDNS.begin(MDNS_HOSTNAME)) MDNS.addService("http", "tcp", 80);
}

// ============================================================================
//  9. HTTP SERVER
// ============================================================================
static void startWebServer() {
  gWs.onEvent(onWsEvent);
  gServer.addHandler(&gWs);

  gServer.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
    // Streams directly from flash; no heap copy of the page
    req->send(200, "text/html", (const uint8_t*)DASHBOARD_HTML,
              sizeof(DASHBOARD_HTML) - 1);
  });

  // Plain JSON snapshot (handy for curl / scripts)
  gServer.on("/api/stats", HTTP_GET, [](AsyncWebServerRequest* req) {
    char out[256];
    snprintf(out, sizeof(out),
             "{\"frames\":%lu,\"bytes\":%lu,\"lenErr\":%lu,\"tailErr\":%lu,"
             "\"dropped\":%lu,\"heap\":%lu,\"up\":%lu}",
             (unsigned long)gStats.framesOk, (unsigned long)gStats.bytesRx,
             (unsigned long)gStats.errLength, (unsigned long)gStats.errTail,
             (unsigned long)gStats.queueDropped,
             (unsigned long)ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
    req->send(200, "application/json", out);
  });

  gServer.onNotFound([](AsyncWebServerRequest* req) {
    req->send(404, "text/plain", "Not found");
  });

  gServer.begin();
  Serial.println("[HTTP] Server started on port 80, WebSocket at /ws");
}

// ============================================================================
//  10. SETUP / LOOP
// ============================================================================
void setup() {
#if !ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxBufferSize(4096);               // smooth out debug bursts
#endif
  Serial.begin(DEBUG_BAUD);
  delay(300);
  Serial.println(F("\n========================================"));
  Serial.println(F("  HLK-LD2451 Radar -> ESP32 Dashboard"));
  Serial.println(F("========================================"));

  // --- Radar UART: buffer size MUST be set before begin() ---
  RadarSerial.setRxBufferSize(RADAR_RX_BUFFER_SIZE);
  RadarSerial.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);
  Serial.printf("[UART] Radar on UART%d  RX=GPIO%d  TX=GPIO%d  %d baud\n",
                RADAR_UART_NUM, RADAR_RX_PIN, RADAR_TX_PIN, RADAR_BAUD);

  gFrameQueue = xQueueCreate(FRAME_QUEUE_DEPTH, sizeof(RadarFrame));
  configASSERT(gFrameQueue);

  // Start parsing BEFORE Wi-Fi so no bytes are missed during connection
  xTaskCreatePinnedToCore(radarRxTask, "radarRx", 4096, nullptr,
                          10, nullptr, 1);

  startWiFi();
  startWebServer();
  printHelp();
}

void loop() {
  const uint32_t now = millis();

  // 1) Drain parsed frames: debug-print every one, keep the newest for the UI
  RadarFrame f;
  while (xQueueReceive(gFrameQueue, &f, 0) == pdTRUE) {
    if (gDebugPretty) printPrettyFrame(f);
    if (gDebugRawHex) printHexFrame(f);
    gLatest        = f;
    gLatestPending = true;
    gLastFrameMs   = now;
    gStaleSent     = false;
  }

  // 2) Throttled real-time push (latest frame wins)
  if (gLatestPending && now - gLastWsTxMs >= WS_MIN_INTERVAL_MS) {
    wsBroadcastFrame(gLatest);
    gLatestPending = false;
    gLastWsTxMs    = now;
  }

  // 3) Stale detection: radar silent -> tell UI to clear targets once
  bool stale = (gLastFrameMs == 0) || (now - gLastFrameMs > STALE_TIMEOUT_MS);
  if (stale && !gStaleSent && gLastFrameMs != 0) {
    Serial.println("[RADAR] No frames for 2 s - check wiring / 5 V supply");
    wsBroadcastStatus(true);
    gStaleSent = true;
  }

  // 4) 1 Hz housekeeping
  if (gLastStatusMs == 0 || now - gLastStatusMs >= STATUS_INTERVAL_MS) {
    gLastStatusMs = now;
    wsBroadcastStatus(stale);
    gWs.cleanupClients();                      // free dead sockets
  }

  handleSerialCommands();
  vTaskDelay(pdMS_TO_TICKS(1));                // yield; keeps IDLE watchdog fed
}
