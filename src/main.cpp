#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>

// ================== TFT PINS (ESP32-C3) ==================
#define PIN_SCK  4
#define PIN_MOSI 6
#define PIN_CS   7
#define PIN_DC   10
#define PIN_RST  1

Adafruit_GC9A01A tft(PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16 canvas(240, 240);

// ================== COLORS ==================
#define BLACK 0x0000
#define WHITE 0xFFFF
#define BLUE  0x001F

// ================== ESPNOW CONFIG ==================
// Must match slaves
static const uint8_t ESPNOW_CHANNEL = 1;

// ================== OPTIONAL NTP (TIME SYNC) ==================
// If you want real schedule time, fill these.
// If left empty, TIME_SYNC will be sent as valid=0 (slaves won't arm schedules).
static const char* WIFI_SSID = "H6645P-75235224_2.4GHz";
static const char* WIFI_PASS = "zQ3Y7Q4RDt";

// Europe/Rome timezone (CET/CEST)
static const char* TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";

// ================== PACKETS ==================
#pragma pack(push, 1)
typedef struct {
  float t;
  float h;
  uint8_t soil;
  uint8_t batt;
  uint8_t r1, r2, r3;
  uint8_t presence;
  uint32_t ms;
} TelemetryPacket;

typedef struct {
  uint8_t type;   // 2
  uint8_t ch;     // 1..13
  uint32_t ms;
} HelloPacket;

typedef struct {
  uint8_t type;          // 6
  uint16_t minuteOfDay;  // 0..1439
  uint8_t weekdayMon0;   // 0..6 (Mon=0)
  uint8_t valid;         // 1=valid, 0=invalid
  uint32_t ms;
} TimeSyncPacket;

typedef struct {
  uint8_t type;   // 1
  uint8_t r1;     // 0/1/2(toggle)/255 keep
  uint8_t r2;     // 0/1/2(toggle)/255 keep
  uint8_t r3;     // 0/1/2(toggle)/255 keep
  uint8_t irrig;  // 1 show "IRRIGA"
  uint16_t liveSec;
  uint32_t ms;
} CommandPacket;

typedef struct {
  uint8_t hh;
  uint8_t mm;
  uint8_t state;    // 0 OFF, 1 ON
  uint8_t daysMask; // bit0 lun ... bit6 dom
} ScheduleRule;

static const uint8_t MAX_SCHEDULE_RULES = 10;

typedef struct {
  uint8_t type;     // 3
  uint8_t relayIdx; // 1..3
  uint8_t count;    // 0..10
  ScheduleRule rules[MAX_SCHEDULE_RULES];
  uint32_t ms;
} ScheduleSyncPacket;
#pragma pack(pop)

// ================== RX STATE ==================
volatile bool hasPkt = false;
TelemetryPacket rx;
TelemetryPacket viewPkt;
unsigned long lastPktAt = 0;
volatile uint32_t rxCount = 0;

// ================== EYES UI ==================
struct Eye { float cx, cy; float w, h; };
Eye L = { 80, 120, 70, 95 };
Eye R = {160, 120, 70, 95 };

float lookX = 0, lookY = 0;
float targetX = 0, targetY = 0;

bool blinking = false;
uint32_t blinkStart = 0;
uint32_t nextBlink = 0;

float ease(float t) { return t * t * (3 - 2 * t); }
void scheduleBlink() { nextBlink = millis() + random(2000, 5000); }

void drawEye(const Eye& e, float lx, float ly, float blinkAmt) {
  float open = 1.0f - blinkAmt;
  float ew = e.w;
  float eh = e.h * open;

  canvas.fillRoundRect(
    (int)(e.cx - ew/2),
    (int)(e.cy - eh/2),
    (int)ew,
    (int)eh,
    (int)(ew * 0.5f),
    BLUE
  );

  float px = e.cx + lx * 18;
  float py = e.cy + ly * 12;

  canvas.fillRoundRect(
    (int)(px - 18),
    (int)(py - 22),
    36,
    44,
    18,
    BLACK
  );
}

// ================== PEERS (learn from RX) ==================
static const uint8_t MAX_PEERS = 10;
struct PeerSlot {
  bool used = false;
  uint8_t mac[6] = {0};
  uint32_t lastSeenMs = 0;
};
PeerSlot peers[MAX_PEERS];

bool macEqual(const uint8_t a[6], const uint8_t b[6]) {
  for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
  return true;
}

int findPeerSlot(const uint8_t mac[6]) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (peers[i].used && macEqual(peers[i].mac, mac)) return i;
  }
  return -1;
}

int allocPeerSlot(const uint8_t mac[6]) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) {
      peers[i].used = true;
      memcpy(peers[i].mac, mac, 6);
      peers[i].lastSeenMs = millis();
      return i;
    }
  }
  return -1;
}

bool addPeerIfNeeded(const uint8_t mac[6]) {
  int idx = findPeerSlot(mac);
  if (idx >= 0) {
    peers[idx].lastSeenMs = millis();
    return true;
  }
  idx = allocPeerSlot(mac);
  if (idx < 0) return false;

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;

  esp_now_del_peer(mac);
  return (esp_now_add_peer(&peer) == ESP_OK);
}

// broadcast peer needed for HELLO
static const uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// ================== ESPNOW RX CALLBACK ==================
void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  rxCount++;

  // learn sender as peer (for time sync / commands)
  addPeerIfNeeded(mac);

  if (len == (int)sizeof(TelemetryPacket)) {
    memcpy((void*)&rx, data, sizeof(TelemetryPacket));
    hasPkt = true;
    lastPktAt = millis();
  }
}

// ================== ESPNOW INIT ==================
bool initEspNow() {
  WiFi.mode(WIFI_STA);
  delay(50);

  esp_wifi_start();

  // force channel
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) return false;

  esp_now_register_recv_cb(onEspNowRecv);

  // add broadcast peer
  esp_now_peer_info_t bc = {};
  memcpy(bc.peer_addr, BCAST_MAC, 6);
  bc.channel = ESPNOW_CHANNEL;
  bc.encrypt = false;
  esp_now_del_peer(BCAST_MAC);
  esp_now_add_peer(&bc);

  return true;
}

// ================== NTP TIME ==================
bool timeSynced = false;

void maybeInitWifiAndNtp() {
  if (strlen(WIFI_SSID) == 0) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) {
    delay(100);
  }
  if (WiFi.status() != WL_CONNECTED) return;

  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");

  // wait for time
  for (int i = 0; i < 50; i++) {
    struct tm tmNow;
    if (getLocalTime(&tmNow, 200)) { timeSynced = true; break; }
    delay(100);
  }
}

bool buildTimeSync(TimeSyncPacket &ts) {
  ts.type = 6;
  ts.ms = millis();

  if (!timeSynced) {
    struct tm tmNow;
    if (getLocalTime(&tmNow, 10)) timeSynced = true;
  }

  if (!timeSynced) {
    ts.valid = 0;
    ts.minuteOfDay = 0;
    ts.weekdayMon0 = 0;
    return false;
  }

  struct tm tmNow;
  if (!getLocalTime(&tmNow, 10)) {
    ts.valid = 0;
    return false;
  }

  int minuteOfDay = tmNow.tm_hour * 60 + tmNow.tm_min;
  // tm_wday: 0=Sun..6=Sat  -> convert to Mon=0..Sun=6
  int w = tmNow.tm_wday;              // Sun=0
  int mon0 = (w == 0) ? 6 : (w - 1);  // Mon=0

  ts.valid = 1;
  ts.minuteOfDay = (uint16_t)minuteOfDay;
  ts.weekdayMon0 = (uint8_t)mon0;
  return true;
}

// ================== TX HELPERS ==================
void sendHello() {
  HelloPacket h{};
  h.type = 2;
  h.ch = ESPNOW_CHANNEL;
  h.ms = millis();
  esp_now_send(BCAST_MAC, (uint8_t*)&h, sizeof(h));
}

void sendTimeSyncToPeers() {
  TimeSyncPacket ts{};
  buildTimeSync(ts);

  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) continue;
    esp_now_send(peers[i].mac, (uint8_t*)&ts, sizeof(ts));
  }
}

void sendCommandToPeers(const CommandPacket &cmd) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) continue;
    esp_now_send(peers[i].mac, (uint8_t*)&cmd, sizeof(cmd));
  }
}

void sendScheduleToPeers(const ScheduleSyncPacket &sync) {
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) continue;
    esp_now_send(peers[i].mac, (uint8_t*)&sync, sizeof(sync));
  }
}

// ================== SERIAL COMMANDS (optional) ==================
// CMD:  cmd r1 r2 r3 irrig liveSec
// Example: cmd 1 255 255 1 600
// SCHED: sched relayIdx count  (then count lines: hh mm state daysMaskBin)
// Example:
// sched 1 2
// 07 30 1 1111100
// 21 00 0 1111111
bool readLine(String &out) {
  static String buf;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { out = buf; buf = ""; return true; }
    buf += c;
    if (buf.length() > 200) buf = ""; // safety
  }
  return false;
}

uint8_t parseDaysMask(const String &s) {
  // expects "1111111" (lun..dom)
  if (s.length() < 7) return 0;
  uint8_t m = 0;
  for (int i = 0; i < 7; i++) {
    if (s[i] == '1') m |= (1 << i);
  }
  return m;
}

void handleSerial() {
  String line;
  if (!readLine(line)) return;
  line.trim();
  if (line.length() == 0) return;

  // tokenize
  Vector<String> tok; // not available; keep simple manual parse
}

// ================== OVERLAY ==================
void drawOverlay(const TelemetryPacket& p, bool linkOk, uint32_t peersCount) {
  canvas.setTextColor(WHITE);
  canvas.setTextWrap(false);

  canvas.setTextSize(2);
  canvas.setCursor(10, 10);
  canvas.print("EVE");

  canvas.setTextSize(1);
  canvas.setCursor(10, 30);
  canvas.print("ESP-NOW: ");
  canvas.print(linkOk ? "OK" : "NO DATA");

  canvas.setCursor(10, 40);
  canvas.print("RX: ");
  canvas.print((unsigned long)rxCount);

  canvas.setCursor(10, 50);
  canvas.print("PEERS: ");
  canvas.print((unsigned long)peersCount);

  canvas.setTextSize(2);
  canvas.setCursor(10, 70);
  if (isnan(p.t)) canvas.print("T --.-C");
  else canvas.printf("T %.1fC", p.t);

  canvas.setCursor(10, 95);
  if (isnan(p.h)) canvas.print("H --%");
  else canvas.printf("H %d%%", (int)(p.h + 0.5f));

  canvas.setCursor(10, 120);
  canvas.printf("S %d%%", p.soil);

  canvas.setCursor(10, 145);
  canvas.printf("B %d%%", p.batt);

  canvas.setTextSize(1);
  canvas.setCursor(10, 170);
  canvas.printf("R1:%d R2:%d R3:%d PIR:%d", p.r1, p.r2, p.r3, p.presence);

  canvas.setCursor(10, 185);
  canvas.print(timeSynced ? "TIME: OK" : "TIME: N/A");
}

uint32_t countPeers() {
  uint32_t c = 0;
  for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used) c++;
  return c;
}

void setup() {
  randomSeed(esp_random());
  Serial.begin(115200);
  delay(800);

  Serial.print("MASTER MAC: ");
  Serial.println(WiFi.macAddress());
  Serial.print("ESPNOW channel: ");
  Serial.println(ESPNOW_CHANNEL);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  tft.begin();
  tft.setRotation(0);

  canvas.fillScreen(BLACK);
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);

  if (!initEspNow()) {
    Serial.println("ESP-NOW init FAIL");
  } else {
    Serial.println("ESP-NOW OK");
  }

  memset((void*)&viewPkt, 0, sizeof(viewPkt));
  viewPkt.t = NAN;
  viewPkt.h = NAN;

  // optional WiFi+NTP
  maybeInitWifiAndNtp();

  scheduleBlink();
}

void loop() {
  uint32_t now = millis();

  // copy packet received
  if (hasPkt) {
    noInterrupts();
    viewPkt = rx;
    hasPkt = false;
    interrupts();
  }

  // periodic HELLO (for slaves channel lock)
  static uint32_t lastHello = 0;
  if (now - lastHello >= 800) {
    lastHello = now;
    sendHello();
  }

  // periodic TIME SYNC (if available)
  static uint32_t lastTimeSync = 0;
  if (now - lastTimeSync >= 5000) {
    lastTimeSync = now;
    sendTimeSyncToPeers();
  }

  // eyes random look
  if (random(0, 100) < 2) {
    targetX = random(-10, 11) / 10.0f;
    targetY = random(-6, 7) / 10.0f;
  }

  lookX += (targetX - lookX) * 0.12f;
  lookY += (targetY - lookY) * 0.12f;

  // blink
  float blinkAmt = 0.0f;
  if (!blinking && now > nextBlink) {
    blinking = true;
    blinkStart = now;
  }
  if (blinking) {
    float t = (now - blinkStart) / 180.0f;
    if (t >= 1.0f) {
      blinking = false;
      scheduleBlink();
    } else {
      blinkAmt = ease(t < 0.5 ? t*2 : (1 - t)*2);
    }
  }

  // render
  canvas.fillScreen(BLACK);
  drawEye(L, lookX, lookY, blinkAmt);
  drawEye(R, lookX, lookY, blinkAmt);

  bool linkOk = (now - lastPktAt) <= 5000;
  drawOverlay(viewPkt, linkOk, countPeers());

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);

  delay(16);
}
