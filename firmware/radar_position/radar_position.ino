/*
  도와드림 LD6001A 위치 출력 펌웨어 (ESP32-S3)
  -----------------------------------------------
  센서를 AT+DEBUG=2 (사람 좌표 + 점 구름) 모드로 켜고, 받은 TLV 프레임을 풀어서
  PC(COM8, 115200)에 아래 줄로 내보냅니다. radar-monitor 사이트가 이 줄을 그립니다.

    P,<x_mm>,<y_mm>,<z_mm>               점 구름의 점 1개 (프레임당 최대 MAX_POINTS개)
    T,<id>,<x_mm>,<y_mm>,<z_mm>,<vx_mm>,<vy_mm>   사람 1명 (센서가 추적 중인 사람)
    F,<frame>,<사람 수>,<점 수>,<millis>  프레임 끝
    S,<3초간 받은 바이트>,<정상 프레임>,<버린 프레임>   3초마다 상태
    # ...                                 안내 메시지

  배선: 센서 TX -> GPIO16, 센서 RX <- GPIO17, GND 공통 (123.ino와 동일)
  프레임 형식 참고: github.com/thieryus007-cloud/HLK-6001A PROTOCOL.md
    HEAD 01..08 | LENGTH u32 | FRAME u32 | TLV1=1 | POINTLEN | 점 25B*n | TLV2=2 | TRACKLEN | 사람 32B*n
    사람 32B: Q u32, ID u32, X Y Z f32 (m), Vx Vy Vz f32 (m/s)
*/
#include <Arduino.h>

HardwareSerial radar(1);
#define RADAR_RX 16
#define RADAR_TX 17
const uint32_t RADAR_BAUD = 115200;
const int MAX_POINTS = 25;

static uint8_t buf[6144];
static size_t blen = 0;
uint32_t rxBytes = 0, okFrames = 0, badFrames = 0, lastStat = 0, lastOk = 0;

static const uint8_t HEAD[8] = {1, 2, 3, 4, 5, 6, 7, 8};

uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
float f32(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
int mm(float m) { return isfinite(m) ? (int)lroundf(m * 1000.0f) : 0; }

void startRadar() {
  Serial.println("# 센서 설정: AT+STOP -> AT+DEBUG=2 -> AT+START");
  radar.print("AT+STOP\n");    delay(300);
  radar.print("AT+DEBUG=2\n"); delay(300);
  radar.print("AT+START\n");   delay(300);
}

void dropFront(size_t n) {
  if (n >= blen) { blen = 0; return; }
  memmove(buf, buf + n, blen - n);
  blen -= n;
}

// 버퍼에서 프레임 하나를 찾으면 처리하고 true
bool parseOne() {
  // 헤더 찾기
  size_t h = 0;
  while (h + 8 <= blen && memcmp(buf + h, HEAD, 8) != 0) h++;
  if (h + 8 > blen) {               // 헤더 없음: 마지막 7바이트만 남김
    if (blen > 7) dropFront(blen - 7);
    return false;
  }
  if (h) dropFront(h);
  if (blen < 32) return false;

  uint32_t len = u32(buf + 8);
  uint32_t frame = u32(buf + 12);
  if (len < 32 || len > sizeof(buf)) { badFrames++; dropFront(1); return true; }
  if (blen < len) return false;      // 아직 덜 옴

  uint32_t tlv1 = u32(buf + 16), plen = u32(buf + 20);
  bool ok = tlv1 == 1 && plen % 25 == 0 && 24 + plen + 8 <= len;
  uint32_t tlen = 0;
  if (ok) {
    uint32_t tlv2 = u32(buf + 24 + plen);
    tlen = u32(buf + 28 + plen);
    ok = tlv2 == 2 && tlen % 32 == 0 && 24 + plen + 8 + tlen == len;
  }
  if (!ok) { badFrames++; dropFront(1); return true; }

  // 점 구름 (많으면 고르게 건너뛰며 MAX_POINTS개까지)
  int np = plen / 25;
  int stepP = np > MAX_POINTS ? (np + MAX_POINTS - 1) / MAX_POINTS : 1;
  for (int i = 0; i < np; i += stepP) {
    const uint8_t *p = buf + 24 + i * 25;
    Serial.printf("P,%d,%d,%d\n", mm(f32(p)), mm(f32(p + 4)), mm(f32(p + 8)));
  }
  // 사람
  int nt = tlen / 32;
  const uint8_t *t0 = buf + 32 + plen;
  for (int i = 0; i < nt; i++) {
    const uint8_t *t = t0 + i * 32;
    Serial.printf("T,%lu,%d,%d,%d,%d,%d\n", (unsigned long)u32(t + 4),
                  mm(f32(t + 8)), mm(f32(t + 12)), mm(f32(t + 16)), mm(f32(t + 20)), mm(f32(t + 24)));
  }
  Serial.printf("F,%lu,%d,%d,%lu\n", (unsigned long)frame, nt, np, (unsigned long)millis());
  okFrames++; lastOk = millis();
  dropFront(len);
  return true;
}

void setup() {
  Serial.begin(115200);
  radar.setRxBufferSize(8192);
  radar.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX, RADAR_TX);
  delay(1500);
  Serial.println("# 도와드림 LD6001A 위치 출력 펌웨어");
  startRadar();
  lastOk = millis();
}

void loop() {
  while (radar.available()) {
    int c = radar.read();
    rxBytes++;
    if (blen < sizeof(buf)) buf[blen++] = (uint8_t)c;
    else { dropFront(sizeof(buf) / 2); badFrames++; }
  }
  while (parseOne()) {}

  // PC에서 'r' 을 보내면 센서 다시 설정
  if (Serial.available()) { char c = Serial.read(); if (c == 'r') startRadar(); }

  if (millis() - lastStat >= 3000) {
    lastStat = millis();
    Serial.printf("S,%lu,%lu,%lu\n", (unsigned long)rxBytes, (unsigned long)okFrames, (unsigned long)badFrames);
    if (rxBytes > 0 && okFrames == 0 && blen >= 16) {   // 형식이 다르면 앞부분을 보여줌
      Serial.print("# 해석 못 한 데이터 앞부분:");
      for (size_t i = 0; i < min(blen, (size_t)32); i++) Serial.printf(" %02X", buf[i]);
      Serial.println();
    }
    rxBytes = okFrames = badFrames = 0;
    if (millis() - lastOk > 10000) { Serial.println("# 10초간 프레임 없음 -> 센서 다시 설정"); startRadar(); lastOk = millis(); }
  }
}
