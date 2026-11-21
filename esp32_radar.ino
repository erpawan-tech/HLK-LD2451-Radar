/******************************************************************************
 *  esp32_radar.ino  -  HLK-LD2451 24 GHz radar  ->  JSON over USB Serial
 *  --------------------------------------------------------------------------
 *  Radar  : UART2  RX = GPIO16 (<- LD2451 TX)   TX = GPIO17 (-> LD2451 RX)
 *           115200 8N1
 *  Output : USB Serial 115200, one JSON object per line ("JSON Lines").
 *           Open index.html in Chrome/Edge and click "Connect ESP32 (USB)".
 *           (Close the Arduino Serial Monitor first - only one app can own
 *           the COM port.)
 *
 *  Output line types
 *   {"type":"frame","seq":12,"ms":34567,"alarm":1,
 *    "targets":[{"id":1,"angle":-5,"dist":42,"dir":1,"speed":55,"snr":120}],
 *    "raw":"F4 F3 F2 F1 07 00 01 01 7B 2A 01 37 78 F8 F7 F6 F5"}
 *   {"type":"status","ms":..,"frames":..,"bytes":..,"lenErr":..,
 *    "tailErr":..,"txDrop":..,"raw":true}
 *   Lines starting with '#' are human-readable info (ignored by dashboard).
 *
 *  Commands accepted on USB Serial (the dashboard can send them too)
 *   'x' toggle raw hex field   's' print status now   '?' help
 *
 *  HARDWARE (HLK-LD2451 manual v1.0): power the module from 5 V capable of
 *  >= 300 mA (not the ESP32 3V3 pin). Its UART is 3.3 V -> direct wiring OK.
 *  Common GND is mandatory.
 *
 *  No external libraries required. Board: any ESP32 / ESP32-S3 (core 2.x/3.x)
 ******************************************************************************/

#include <Arduino.h>

// ----------------------------- configuration -------------------------------
#define RADAR_RX_PIN          16
#define RADAR_TX_PIN          17
#define RADAR_BAUD            115200
#define USB_BAUD              115200
#define RADAR_RX_BUFFER_SIZE  4096   // ~350 ms of radar data at 115200
#define USB_TX_BUFFER_SIZE    4096
#define EMPTY_FRAME_MIN_MS    200    // rate-limit "no target" frames (5 Hz)
#define STATUS_INTERVAL_MS    1000

static bool gSendRaw = true;         // include "raw" hex field (toggle: 'x')

// ----------------------------- protocol ------------------------------------
//  F4 F3 F2 F1 | LEN(2, LE) | COUNT | ALARM | COUNT x [ang dist dir spd snr] | F8 F7 F6 F5
//  LEN = bytes between LEN field and tail. LEN == 0 -> no targets.
static const uint8_t LD_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t LD_TAIL[4]   = {0xF8, 0xF7, 0xF6, 0xF5};

#define LD_MAX_TARGETS   10
#define LD_TARGET_BYTES  5
#define LD_MAX_PAYLOAD   (2 + LD_MAX_TARGETS * LD_TARGET_BYTES)
#define LD_MAX_FRAME     (4 + 2 + LD_MAX_PAYLOAD + 4)

struct RadarTarget {
  int8_t  angleDeg;      // raw - 0x80
  uint8_t distanceM;
  uint8_t approaching;   // 1 = approaching, 0 = moving away
  uint8_t speedKmh;
  uint8_t snr;
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

struct Stats {
  uint32_t bytesRx = 0, framesOk = 0, errLength = 0, errTail = 0, txDropped = 0;
} gStats;

// ------------------- non-blocking byte-level state machine ------------------
//  feed() handles exactly one byte and returns immediately; true = a complete,
//  validated frame was decoded into `out`. Any unexpected byte resynchronises
//  to header hunting (header bytes are distinct, so one-step restart is exact).
class LD2451Parser {
 public:
  bool feed(uint8_t b, RadarFrame& out) {
    switch (state_) {
      case State::Header:
        if (b == LD_HEADER[idx_]) {
          buf_[idx_++] = b;
          if (idx_ == 4) { rawLen_ = 4; state_ = State::LenLo; }
        } else {
          idx_ = 0;
          if (b == LD_HEADER[0]) { buf_[0] = b; idx_ = 1; }
        }
        break;

      case State::LenLo:
        buf_[rawLen_++] = b; len_ = b; state_ = State::LenHi;
        break;

      case State::LenHi:
        buf_[rawLen_++] = b;
        len_ |= (uint16_t)b << 8;
        if (len_ == 0)                             { idx_ = 0; state_ = State::Tail; }
        else if (len_ < 2 || len_ > LD_MAX_PAYLOAD) { gStats.errLength++; reset(); }
        else                                        { idx_ = 0; state_ = State::Payload; }
        break;

      case State::Payload:
        buf_[rawLen_++] = b;
        if (++idx_ >= len_) { idx_ = 0; state_ = State::Tail; }
        break;

      case State::Tail:
        if (b == LD_TAIL[idx_]) {
          buf_[rawLen_++] = b;
          if (++idx_ == 4) { bool ok = decode(out); reset(); return ok; }
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
    const uint8_t* p = &buf_[6];
    out.timestampMs = millis();
    out.seq = ++seq_;
    out.targetCount = 0;
    out.alarm = 0;
    if (len_ >= 2) {
      uint8_t count = p[0];
      if (count > LD_MAX_TARGETS || (uint16_t)(2 + count * LD_TARGET_BYTES) > len_) {
        gStats.errLength++;
        return false;
      }
      out.targetCount = count;
      out.alarm = p[1];
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

  State    state_ = State::Header;
  uint8_t  idx_ = 0;
  uint16_t len_ = 0;
  uint8_t  rawLen_ = 0;
  uint32_t seq_ = 0;
  uint8_t  buf_[LD_MAX_FRAME];
};

// ----------------------------- globals -------------------------------------
static HardwareSerial RadarSerial(2);
static LD2451Parser   gParser;
static RadarFrame     gFrame;
static uint32_t       gLastEmptyTx = 0;
static bool           gEmptySentOnce = false;
static uint32_t       gLastStatus  = 0;

// ----------------------------- JSON output ---------------------------------
// Never blocks: if the USB TX buffer can't take the whole line, the frame is
// skipped (counted in txDrop) so radar reception is never stalled.
static bool writeLine(const char* s, size_t n) {
  if ((size_t)Serial.availableForWrite() < n + 2) { gStats.txDropped++; return false; }
  Serial.write((const uint8_t*)s, n);
  Serial.write("\r\n", 2);
  return true;
}

static void sendFrameJson(const RadarFrame& f) {
  static char line[96 + LD_MAX_TARGETS * 72 + LD_MAX_FRAME * 3];
  int pos = snprintf(line, sizeof(line),
                     "{\"type\":\"frame\",\"seq\":%lu,\"ms\":%lu,\"alarm\":%u,\"targets\":[",
                     (unsigned long)f.seq, (unsigned long)f.timestampMs, f.alarm);
  for (uint8_t i = 0; i < f.targetCount; i++) {
    const RadarTarget& t = f.targets[i];
    pos += snprintf(line + pos, sizeof(line) - pos,
                    "%s{\"id\":%u,\"angle\":%d,\"dist\":%u,\"dir\":%u,\"speed\":%u,\"snr\":%u}",
                    i ? "," : "", i + 1, t.angleDeg, t.distanceM, t.approaching,
                    t.speedKmh, t.snr);
  }
  pos += snprintf(line + pos, sizeof(line) - pos, "]");
  if (gSendRaw) {
    pos += snprintf(line + pos, sizeof(line) - pos, ",\"raw\":\"");
    for (uint8_t i = 0; i < f.rawLen; i++)
      pos += snprintf(line + pos, sizeof(line) - pos, i ? " %02X" : "%02X", f.raw[i]);
    pos += snprintf(line + pos, sizeof(line) - pos, "\"");
  }
  pos += snprintf(line + pos, sizeof(line) - pos, "}");
  writeLine(line, pos);
}

static void sendStatusJson() {
  char line[200];
  int n = snprintf(line, sizeof(line),
                   "{\"type\":\"status\",\"ms\":%lu,\"frames\":%lu,\"bytes\":%lu,"
                   "\"lenErr\":%lu,\"tailErr\":%lu,\"txDrop\":%lu,\"raw\":%s}",
                   (unsigned long)millis(), (unsigned long)gStats.framesOk,
                   (unsigned long)gStats.bytesRx, (unsigned long)gStats.errLength,
                   (unsigned long)gStats.errTail, (unsigned long)gStats.txDropped,
                   gSendRaw ? "true" : "false");
  writeLine(line, n);
}

static void handleCommands() {
  while (Serial.available()) {
    switch (Serial.read()) {
      case 'x': gSendRaw = !gSendRaw;
                Serial.printf("# raw hex %s\n", gSendRaw ? "ON" : "OFF"); break;
      case 's': sendStatusJson(); break;
      case '?': Serial.println("# commands: x=toggle raw hex, s=status"); break;
      default: break;
    }
  }
}

// ----------------------------- setup / loop --------------------------------
void setup() {
#if !ARDUINO_USB_CDC_ON_BOOT
  Serial.setTxBufferSize(USB_TX_BUFFER_SIZE);   // must precede begin()
#endif
  Serial.begin(USB_BAUD);

  RadarSerial.setRxBufferSize(RADAR_RX_BUFFER_SIZE);  // must precede begin()
  RadarSerial.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX_PIN, RADAR_TX_PIN);

  delay(200);
  Serial.println("# HLK-LD2451 -> USB JSON bridge ready (UART2 RX=16 TX=17 @115200)");
}

void loop() {
  // 1) Drain the radar UART completely; parsing is O(1) per byte
  uint8_t chunk[128];
  int avail;
  while ((avail = RadarSerial.available()) > 0) {
    size_t n = RadarSerial.read(chunk, min(avail, (int)sizeof(chunk)));
    gStats.bytesRx += n;
    for (size_t i = 0; i < n; i++) {
      if (!gParser.feed(chunk[i], gFrame)) continue;
      const uint32_t now = millis();
      if (gFrame.targetCount == 0) {
        if (gEmptySentOnce && now - gLastEmptyTx < EMPTY_FRAME_MIN_MS) continue;  // rate-limit
        gLastEmptyTx = now;
        gEmptySentOnce = true;
      }
      sendFrameJson(gFrame);
    }
  }

  // 2) 1 Hz health line
  if (millis() - gLastStatus >= STATUS_INTERVAL_MS) {
    gLastStatus = millis();
    sendStatusJson();
  }

  handleCommands();
  delay(1);   // yield to RTOS / feed watchdog
}
