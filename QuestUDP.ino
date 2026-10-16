// Two TF-Lunas (I2C, through TCA9548A mux) -> UDP packets over WiFi to a Quest 3
// Board: Adafruit Feather ESP32-S3 Reverse TFT
//
// Packet format (one packet per sensor per reading):
//   id=luna_1,cm=199,strength=6670,temp=34.0
//
// Wiring (same as MuxTwoLuna):
//   Mux:    VIN -> 3.3V, GND -> GND, SDA -> Feather SDA, SCL -> Feather SCL
//   luna_1: blue -> SD0, green -> SC0
//   luna_2: blue -> SD1, green -> SC1
//   Both:   red -> VBUS, black -> GND, yellow -> GND, white unconnected

#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>

// ======== FILL THESE IN ========
const char* WIFI_SSID = "RL-GLI-2-4";
const char* WIFI_PASS = "RedlineLogic";
IPAddress   QUEST_IP(192, 168, 8, 162);    // Quest 3's IP on the same network
const uint16_t QUEST_PORT = 5005;          // port your Quest app listens on
// ===============================

#define PIN_SDA    3
#define PIN_SCL    4
#define MUX_ADDR   0x70
#define LUNA_ADDR  0x10
#define SEND_EVERY_MS 100                  // 10 packets/sec per sensor

struct Luna { uint8_t id; uint8_t ch; };
Luna lunas[2] = { {1, 0}, {2, 1} };        // luna_1 on ch0, luna_2 on ch1

WiFiUDP udp;
int failStreak = 0;

// ---------- I2C ----------
bool muxSelect(uint8_t ch) {
  Wire.beginTransmission(MUX_ADDR);
  Wire.write(1 << ch);
  return Wire.endTransmission() == 0;
}

// Returns true on success; fills dist (cm), amp, tempC
bool readLuna(uint8_t ch, uint16_t &dist, uint16_t &amp, float &tempC) {
  if (!muxSelect(ch)) return false;

  Wire.beginTransmission(LUNA_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;

  if (Wire.requestFrom(LUNA_ADDR, 6) != 6) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = Wire.read();

  dist  = b[0] | (b[1] << 8);
  amp   = b[2] | (b[3] << 8);
  tempC = (b[4] | (b[5] << 8)) / 100.0;
  return true;
}

void i2cBegin() {
  Wire.begin(PIN_SDA, PIN_SCL);
  Wire.setClock(100000);
}

// Frees a sensor that's stuck holding SDA low (the "mux not answering" lockup):
// clock SCL 9 times, send a STOP, then restart I2C.
void i2cRecover() {
  Serial.println("I2C stuck - recovering bus");
  Wire.end();
  pinMode(PIN_SDA, INPUT_PULLUP);
  pinMode(PIN_SCL, OUTPUT_OPEN_DRAIN);
  for (int i = 0; i < 9; i++) {
    digitalWrite(PIN_SCL, LOW);  delayMicroseconds(10);
    digitalWrite(PIN_SCL, HIGH); delayMicroseconds(10);
  }
  pinMode(PIN_SDA, OUTPUT_OPEN_DRAIN);
  digitalWrite(PIN_SDA, LOW);  delayMicroseconds(10);
  digitalWrite(PIN_SCL, HIGH); delayMicroseconds(10);
  digitalWrite(PIN_SDA, HIGH); delayMicroseconds(10);
  i2cBegin();
}

// ---------- WiFi ----------
void connectWiFi() {
  Serial.printf("Connecting to WiFi \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. Feather IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("Sending to %s:%u\n", QUEST_IP.toString().c_str(), QUEST_PORT);
  } else {
    Serial.println("\nWiFi FAILED - check name/password (2.4 GHz networks only)");
  }
}

void sendPacket(uint8_t id, uint16_t cm, uint16_t strength, float temp) {
  char msg[64];
  snprintf(msg, sizeof(msg), "id=luna_%u,cm=%u,strength=%u,temp=%.1f", id, cm, strength, temp);
  udp.beginPacket(QUEST_IP, QUEST_PORT);
  udp.print(msg);
  udp.endPacket();
  Serial.println(msg);   // also show it locally
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  pinMode(7, OUTPUT);          // I2C power / pull-ups on this Feather
  digitalWrite(7, HIGH);
  delay(50);
  i2cBegin();
  delay(300);

  connectWiFi();
  udp.begin(0);                // any local port is fine for sending
}

void loop() {
  static unsigned long lastSend = 0;
  if (millis() - lastSend < SEND_EVERY_MS) return;
  lastSend = millis();

  // Reconnect if WiFi dropped
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastRetry = 0;
    if (millis() - lastRetry > 5000) {
      lastRetry = millis();
      Serial.println("WiFi lost - reconnecting");
      WiFi.reconnect();
    }
  }

  int okCount = 0;
  for (auto &l : lunas) {
    uint16_t dist, amp;
    float temp;
    if (readLuna(l.ch, dist, amp, temp)) {
      okCount++;
      if (WiFi.status() == WL_CONNECTED) sendPacket(l.id, dist, amp, temp);
    } else {
      Serial.printf("luna_%u: read failed\n", l.id);
    }
  }

  // Both sensors failing for ~0.5 s in a row = probably a locked bus
  failStreak = (okCount == 0) ? failStreak + 1 : 0;
  if (failStreak >= 5) {
    i2cRecover();
    failStreak = 0;
  }
}
