#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <time.h>
#include <Preferences.h>
#include <PubSubClient.h>

#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>

#include "power_schedule_core.h"

// ================== TFT PINS (ESP32-C3) ==================
#define PIN_SCK  4
#define PIN_MOSI 6
#define PIN_CS   7
#define PIN_DC   10
#define PIN_RST  1

Adafruit_GC9A01A tft(PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16 canvas(240, 240);

#define BLACK 0x0000
#define WHITE 0xFFFF
#define BLUE  0x001F

static const uint8_t ESPNOW_CHANNEL = 1;
static const char* WIFI_SSID = "H6645P-75235224_2.4GHz";
static const char* WIFI_PASS = "zQ3Y7Q4RDt";
static const char* TZ_INFO = "CET-1CEST,M3.5.0/2,M10.5.0/3";

static const char* MQTT_HOST = "192.168.1.10";
static const uint16_t MQTT_PORT = 1883;
static const char* MQTT_CLIENT_ID = "eve-power-master";

static const uint32_t SCHEDULE_ACK_TIMEOUT_MS = 3000;
static const uint8_t SCHEDULE_RETRY_MAX = 1;

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

typedef struct { uint8_t type; uint8_t ch; uint32_t ms; } HelloPacket;
typedef struct { uint8_t type; uint16_t minuteOfDay; uint8_t weekdayMon0; uint8_t valid; uint32_t ms; } TimeSyncPacket;
typedef struct { uint8_t type; uint8_t r1; uint8_t r2; uint8_t r3; uint8_t irrig; uint16_t liveSec; uint32_t ms; } CommandPacket;
#pragma pack(pop)

volatile bool hasPkt = false;
TelemetryPacket rx;
TelemetryPacket viewPkt;
unsigned long lastPktAt = 0;
volatile uint32_t rxCount = 0;

struct Eye { float cx, cy; float w, h; };
Eye L = { 80, 120, 70, 95 };
Eye R = {160, 120, 70, 95 };
float lookX = 0, lookY = 0, targetX = 0, targetY = 0;
bool blinking = false;
uint32_t blinkStart = 0, nextBlink = 0;

float ease(float t) { return t * t * (3 - 2 * t); }
void scheduleBlink() { nextBlink = millis() + random(2000, 5000); }

void drawEye(const Eye& e, float lx, float ly, float blinkAmt) {
  float open = 1.0f - blinkAmt;
  float ew = e.w;
  float eh = e.h * open;
  canvas.fillRoundRect((int)(e.cx - ew/2), (int)(e.cy - eh/2), (int)ew, (int)eh, (int)(ew * 0.5f), BLUE);
  float px = e.cx + lx * 18;
  float py = e.cy + ly * 12;
  canvas.fillRoundRect((int)(px - 18), (int)(py - 22), 36, 44, 18, BLACK);
}

static const uint8_t MAX_PEERS = 10;
struct PeerSlot { bool used = false; uint8_t mac[6] = {0}; uint32_t lastSeenMs = 0; };
PeerSlot peers[MAX_PEERS];

bool macEqual(const uint8_t a[6], const uint8_t b[6]) { for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false; return true; }
int findPeerSlot(const uint8_t mac[6]) { for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used && macEqual(peers[i].mac, mac)) return i; return -1; }
int allocPeerSlot(const uint8_t mac[6]) { for (int i = 0; i < MAX_PEERS; i++) if (!peers[i].used) { peers[i].used = true; memcpy(peers[i].mac, mac, 6); peers[i].lastSeenMs = millis(); return i; } return -1; }

bool addPeerIfNeeded(const uint8_t mac[6]) {
  int idx = findPeerSlot(mac);
  if (idx >= 0) { peers[idx].lastSeenMs = millis(); return true; }
  idx = allocPeerSlot(mac);
  if (idx < 0) return false;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  esp_now_del_peer(mac);
  return (esp_now_add_peer(&peer) == ESP_OK);
}

static const uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
Preferences prefs;

PowerRelaySchedule activeSchedules[3];
PowerRelaySchedule pendingSchedules[3];
bool waitingAck[3] = {false,false,false};
uint8_t retryLeft[3] = {0,0,0};
uint32_t ackDeadline[3] = {0,0,0};
uint8_t relayState[3] = {0,0,0};

String topicForRelay(uint8_t relay, const char* suffix) {
  char b[96];
  snprintf(b, sizeof(b), "progetto/EVE/POWER/relay/%u/%s", relay, suffix);
  return String(b);
}

void mqttPublish(uint8_t relay, const char* suffix, const String &payload, bool retained = false) {
  mqtt.publish(topicForRelay(relay, suffix).c_str(), payload.c_str(), retained);
}

void persistSchedule(uint8_t relay, const PowerRelaySchedule &s) {
  prefs.putString((String("schedule_") + relay).c_str(), scheduleToJson(s).c_str());
}

void loadSchedules() {
  for (uint8_t r = 1; r <= 3; r++) {
    std::string err;
    String raw = prefs.getString((String("schedule_") + r).c_str(), "[]");
    if (!parseScheduleJson(raw.c_str(), activeSchedules[r-1], err)) activeSchedules[r-1].count = 0;
  }
}

void publishRetainedSchedules() {
  for (uint8_t r = 1; r <= 3; r++) {
    mqttPublish(r, "schedule/current", scheduleToJson(activeSchedules[r-1]).c_str(), true);
  }
}

bool sendRulesPacket(uint8_t relay, const PowerRelaySchedule &schedule) {
  PowerRelayRulesPacket pkt{};
  if (!buildRulesPacket(relay, schedule, millis(), pkt)) return false;
  bool sent = false;
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!peers[i].used) continue;
    if (esp_now_send(peers[i].mac, (uint8_t*)&pkt, sizeof(pkt)) == ESP_OK) sent = true;
  }
  Serial.printf("[SCHEDULE] relay=%u send type14 count=%u sent=%d\n", relay, pkt.count, sent);
  return sent;
}

void handleScheduleAck(const PowerScheduleAckPacket &ack) {
  if (ack.ch < 1 || ack.ch > 3) return;
  uint8_t idx = ack.ch - 1;
  if (!waitingAck[idx]) return;

  waitingAck[idx] = false;
  if (ack.ok == 1) {
    activeSchedules[idx] = pendingSchedules[idx];
    persistSchedule(ack.ch, activeSchedules[idx]);
    mqttPublish(ack.ch, "schedule/slave/ack", "OK", false);
    mqttPublish(ack.ch, "schedule", "OK SCHEDULAZIONE", true);
    mqttPublish(ack.ch, "schedule/current", scheduleToJson(activeSchedules[idx]).c_str(), true);
  } else {
    mqttPublish(ack.ch, "schedule/slave/ack", "ERROR", false);
  }
  Serial.printf("[SCHEDULE_ACK] relay=%u ok=%u count=%u ms=%lu\n", ack.ch, ack.ok, ack.count, (unsigned long)ack.ms);
}

void handleExecuted(const PowerExecutedPacket &ex) {
  if (ex.ch < 1 || ex.ch > 3) return;
  relayState[ex.ch - 1] = ex.state ? 1 : 0;
  mqttPublish(ex.ch, "executed", ex.state ? "ON" : "OFF", false);
  mqttPublish(ex.ch, "state", ex.state ? "ON" : "OFF", false);
  Serial.printf("[EXECUTED] relay=%u state=%u minute=%u weekday=%u\n", ex.ch, ex.state, ex.minuteOfDay, ex.weekdayMon0);
}

void handleScheduleSet(uint8_t relay, const String &payload) {
  std::string err;
  PowerRelaySchedule candidate{};
  if (!parseScheduleJson(payload.c_str(), candidate, err)) {
    Serial.printf("[SCHEDULE] relay=%u invalid=%s\n", relay, err.c_str());
    mqttPublish(relay, "schedule/slave/ack", "ERROR", false);
    return;
  }

  if (!waitingAck[relay-1] && schedulesEqual(candidate, activeSchedules[relay-1])) {
    mqttPublish(relay, "schedule/current", scheduleToJson(activeSchedules[relay-1]).c_str(), true);
    Serial.printf("[SCHEDULE] relay=%u idempotent no-op\n", relay);
    return;
  }

  pendingSchedules[relay-1] = candidate;
  waitingAck[relay-1] = true;
  retryLeft[relay-1] = SCHEDULE_RETRY_MAX;
  ackDeadline[relay-1] = millis() + SCHEDULE_ACK_TIMEOUT_MS;

  if (!sendRulesPacket(relay, candidate)) {
    Serial.printf("[SCHEDULE] relay=%u send failed\n", relay);
  }
}

void checkScheduleTimeouts() {
  uint32_t now = millis();
  for (uint8_t relay = 1; relay <= 3; relay++) {
    uint8_t idx = relay - 1;
    if (!waitingAck[idx]) continue;
    if ((int32_t)(now - ackDeadline[idx]) < 0) continue;

    if (retryLeft[idx] > 0) {
      retryLeft[idx]--;
      ackDeadline[idx] = now + SCHEDULE_ACK_TIMEOUT_MS;
      sendRulesPacket(relay, pendingSchedules[idx]);
      Serial.printf("[SCHEDULE] relay=%u retry\n", relay);
    } else {
      waitingAck[idx] = false;
      mqttPublish(relay, "schedule/slave/ack", "ERROR", false);
      Serial.printf("[SCHEDULE] relay=%u timeout\n", relay);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String t = String(topic);
  String p;
  for (unsigned int i = 0; i < length; i++) p += (char)payload[i];

  for (uint8_t relay = 1; relay <= 3; relay++) {
    if (t == topicForRelay(relay, "schedule/set")) {
      handleScheduleSet(relay, p);
      return;
    }
    if (t == topicForRelay(relay, "set")) {
      CommandPacket cmd{};
      cmd.type = 1;
      cmd.r1 = cmd.r2 = cmd.r3 = 255;
      cmd.irrig = 0;
      cmd.liveSec = 60;
      if (p == "ON") cmd.r1 = 1;
      else if (p == "OFF") cmd.r1 = 0;
      else if (p == "TOGGLE") cmd.r1 = 2;
      if (relay == 2) { cmd.r2 = cmd.r1; cmd.r1 = 255; }
      if (relay == 3) { cmd.r3 = cmd.r1; cmd.r1 = 255; }
      for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used) esp_now_send(peers[i].mac, (uint8_t*)&cmd, sizeof(cmd));
      return;
    }
  }
}

void ensureMqttConnected() {
  if (!mqtt.connected()) {
    if (mqtt.connect(MQTT_CLIENT_ID)) {
      for (uint8_t relay = 1; relay <= 3; relay++) {
        mqtt.subscribe(topicForRelay(relay, "set").c_str());
        mqtt.subscribe(topicForRelay(relay, "schedule/set").c_str());
      }
      publishRetainedSchedules();
    }
  }
}

void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  rxCount++;
  addPeerIfNeeded(mac);

  if (len == (int)sizeof(TelemetryPacket)) {
    memcpy((void*)&rx, data, sizeof(TelemetryPacket));
    hasPkt = true;
    lastPktAt = millis();
    return;
  }

  if (len == (int)sizeof(PowerScheduleAckPacket) && data[0] == 15) {
    PowerScheduleAckPacket ack;
    memcpy(&ack, data, sizeof(ack));
    handleScheduleAck(ack);
    return;
  }

  if (len == (int)sizeof(PowerExecutedPacket) && data[0] == 16) {
    PowerExecutedPacket ex;
    memcpy(&ex, data, sizeof(ex));
    handleExecuted(ex);
    return;
  }
}

bool initEspNow() {
  WiFi.mode(WIFI_STA);
  delay(50);
  esp_wifi_start();
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);
  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(onEspNowRecv);
  esp_now_peer_info_t bc = {};
  memcpy(bc.peer_addr, BCAST_MAC, 6);
  bc.channel = ESPNOW_CHANNEL;
  bc.encrypt = false;
  esp_now_del_peer(BCAST_MAC);
  esp_now_add_peer(&bc);
  return true;
}

bool timeSynced = false;
void maybeInitWifiAndNtp() {
  if (strlen(WIFI_SSID) == 0) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(100);
  if (WiFi.status() != WL_CONNECTED) return;
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  for (int i = 0; i < 50; i++) { struct tm tmNow; if (getLocalTime(&tmNow, 200)) { timeSynced = true; break; } delay(100); }
}

bool buildTimeSync(TimeSyncPacket &ts) {
  ts.type = 6;
  ts.ms = millis();
  if (!timeSynced) { struct tm tmNow; if (getLocalTime(&tmNow, 10)) timeSynced = true; }
  if (!timeSynced) { ts.valid = 0; ts.minuteOfDay = 0; ts.weekdayMon0 = 0; return false; }
  struct tm tmNow;
  if (!getLocalTime(&tmNow, 10)) { ts.valid = 0; return false; }
  int minuteOfDay = tmNow.tm_hour * 60 + tmNow.tm_min;
  int w = tmNow.tm_wday;
  int mon0 = (w == 0) ? 6 : (w - 1);
  ts.valid = 1;
  ts.minuteOfDay = (uint16_t)minuteOfDay;
  ts.weekdayMon0 = (uint8_t)mon0;
  return true;
}

void sendHello() { HelloPacket h{}; h.type = 2; h.ch = ESPNOW_CHANNEL; h.ms = millis(); esp_now_send(BCAST_MAC, (uint8_t*)&h, sizeof(h)); }
void sendTimeSyncToPeers() { TimeSyncPacket ts{}; buildTimeSync(ts); for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used) esp_now_send(peers[i].mac, (uint8_t*)&ts, sizeof(ts)); }
uint32_t countPeers() { uint32_t c = 0; for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used) c++; return c; }

void drawOverlay(const TelemetryPacket& p, bool linkOk, uint32_t peersCount) {
  canvas.setTextColor(WHITE); canvas.setTextWrap(false); canvas.setTextSize(2); canvas.setCursor(10, 10); canvas.print("EVE");
  canvas.setTextSize(1); canvas.setCursor(10, 30); canvas.print("ESP-NOW: "); canvas.print(linkOk ? "OK" : "NO DATA");
  canvas.setCursor(10, 40); canvas.print("RX: "); canvas.print((unsigned long)rxCount);
  canvas.setCursor(10, 50); canvas.print("PEERS: "); canvas.print((unsigned long)peersCount);
  canvas.setTextSize(2); canvas.setCursor(10, 70); if (isnan(p.t)) canvas.print("T --.-C"); else canvas.printf("T %.1fC", p.t);
  canvas.setCursor(10, 95); if (isnan(p.h)) canvas.print("H --%"); else canvas.printf("H %d%%", (int)(p.h + 0.5f));
  canvas.setCursor(10, 120); canvas.printf("S %d%%", p.soil); canvas.setCursor(10, 145); canvas.printf("B %d%%", p.batt);
  canvas.setTextSize(1); canvas.setCursor(10, 170); canvas.printf("R1:%d R2:%d R3:%d PIR:%d", p.r1, p.r2, p.r3, p.presence);
  canvas.setCursor(10, 185); canvas.print(timeSynced ? "TIME: OK" : "TIME: N/A");
}

void setup() {
  randomSeed(esp_random());
  Serial.begin(115200);
  delay(800);
  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  tft.begin();
  tft.setRotation(0);
  canvas.fillScreen(BLACK);
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);

  if (!initEspNow()) Serial.println("ESP-NOW init FAIL");
  memset((void*)&viewPkt, 0, sizeof(viewPkt));
  viewPkt.t = NAN;
  viewPkt.h = NAN;

  maybeInitWifiAndNtp();
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  prefs.begin("eve_power", false);
  loadSchedules();

  scheduleBlink();
}

void loop() {
  uint32_t now = millis();
  if (hasPkt) { noInterrupts(); viewPkt = rx; hasPkt = false; interrupts(); }

  static uint32_t lastHello = 0;
  if (now - lastHello >= 800) { lastHello = now; sendHello(); }

  static uint32_t lastTimeSync = 0;
  if (now - lastTimeSync >= 5000) { lastTimeSync = now; sendTimeSyncToPeers(); }

  if (WiFi.status() == WL_CONNECTED) {
    ensureMqttConnected();
    mqtt.loop();
  }
  checkScheduleTimeouts();

  if (random(0, 100) < 2) { targetX = random(-10, 11) / 10.0f; targetY = random(-6, 7) / 10.0f; }
  lookX += (targetX - lookX) * 0.12f;
  lookY += (targetY - lookY) * 0.12f;

  float blinkAmt = 0.0f;
  if (!blinking && now > nextBlink) { blinking = true; blinkStart = now; }
  if (blinking) {
    float t = (now - blinkStart) / 180.0f;
    if (t >= 1.0f) { blinking = false; scheduleBlink(); }
    else blinkAmt = ease(t < 0.5 ? t*2 : (1 - t)*2);
  }

  canvas.fillScreen(BLACK);
  drawEye(L, lookX, lookY, blinkAmt);
  drawEye(R, lookX, lookY, blinkAmt);
  bool linkOk = (now - lastPktAt) <= 5000;
  drawOverlay(viewPkt, linkOk, countPeers());
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);
  delay(16);
}
