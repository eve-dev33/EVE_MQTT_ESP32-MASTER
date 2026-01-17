#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>

// ===== PIN TFT =====
#define PIN_SCK  4
#define PIN_MOSI 6
#define PIN_CS   7
#define PIN_DC   10
#define PIN_RST  1

Adafruit_GC9A01A tft(PIN_CS, PIN_DC, PIN_RST);
GFXcanvas16 canvas(240, 240);

// ===== COLORI =====
#define BLACK 0x0000
#define WHITE 0xFFFF
#define BLUE  0x001F

// ===== ESP-NOW CONFIG =====
static const uint8_t ESPNOW_CHANNEL = 1;

// ===== PACKET =====
typedef struct __attribute__((packed)) {
  float t;
  float h;
  uint8_t soil;
  uint8_t batt;
  uint8_t r1, r2, r3;
  uint8_t presence;
  uint32_t ms;
} TelemetryPacket;

volatile bool hasPkt = false;
TelemetryPacket rx;
TelemetryPacket viewPkt;
unsigned long lastPktAt = 0;

// Debug RX
volatile uint32_t rxCount = 0;

// ===== OCCHI =====
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

// ===== ESP-NOW RX CALLBACK =====
void onEspNowRecv(const uint8_t* mac, const uint8_t* data, int len) {
  rxCount++;

  Serial.print("RX #"); Serial.print(rxCount);
  Serial.print(" from ");
  Serial.printf("%02X:%02X:%02X:%02X:%02X:%02X",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print(" len=");
  Serial.println(len);

  if (len == (int)sizeof(TelemetryPacket)) {
    memcpy((void*)&rx, data, sizeof(TelemetryPacket));
    hasPkt = true;
    lastPktAt = millis();
  } else {
    Serial.println("RX len mismatch");
  }
}

bool initEspNowRx() {
  WiFi.mode(WIFI_STA);
  delay(50);

  // importante: start WiFi stack
  esp_wifi_start();

  // forza canale per allineamento
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) {
    return false;
  }

  esp_now_register_recv_cb(onEspNowRecv);
  return true;
}

void drawOverlay(const TelemetryPacket& p, bool linkOk) {
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

  canvas.setTextSize(2);

  canvas.setCursor(10, 60);
  canvas.printf("T %.1fC", p.t);

  canvas.setCursor(10, 85);
  canvas.printf("H %d%%", (int)(p.h + 0.5f));

  canvas.setCursor(10, 110);
  canvas.printf("S %d%%", p.soil);

  canvas.setCursor(10, 135);
  canvas.printf("B %d%%", p.batt);

  canvas.setTextSize(1);
  canvas.setCursor(10, 160);
  canvas.printf("R1:%d R2:%d R3:%d PIR:%d", p.r1, p.r2, p.r3, p.presence);
}

void setup() {
  randomSeed(esp_random());

  Serial.begin(115200);
  delay(1500);

  Serial.print("C3 MAC: ");
  Serial.println(WiFi.macAddress());

  Serial.print("C3 channel forced to: ");
  Serial.println(ESPNOW_CHANNEL);

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  tft.begin();
  tft.setRotation(0);

  canvas.fillScreen(BLACK);
  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);

  if (!initEspNowRx()) {
    Serial.println("ESP-NOW init FAIL");
  } else {
    Serial.println("ESP-NOW RX OK");
  }

  memset((void*)&viewPkt, 0, sizeof(viewPkt));
  viewPkt.t = NAN;
  viewPkt.h = NAN;
  scheduleBlink();
}

void loop() {
  uint32_t now = millis();

  // copia pacchetto ricevuto
  if (hasPkt) {
    noInterrupts();
    viewPkt = rx;
    hasPkt = false;
    interrupts();
  }

  if (random(0, 100) < 2) {
    targetX = random(-10, 11) / 10.0f;
    targetY = random(-6, 7) / 10.0f;
  }

  lookX += (targetX - lookX) * 0.12f;
  lookY += (targetY - lookY) * 0.12f;

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

  canvas.fillScreen(BLACK);
  drawEye(L, lookX, lookY, blinkAmt);
  drawEye(R, lookX, lookY, blinkAmt);

  bool linkOk = (now - lastPktAt) <= 5000;
  drawOverlay(viewPkt, linkOk);

  tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 240, 240);

  delay(16);
}
