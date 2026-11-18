#include <Arduino.h>

HardwareSerial radar(1);
const uint32_t RADAR_BAUD = 115200;

uint8_t frame[10];
size_t used = 0;
uint32_t received = 0;
uint32_t validFrames = 0;
uint32_t lastReport = 0;

void setup() {
  Serial.begin(115200);
  radar.begin(RADAR_BAUD, SERIAL_8N1, 16, 17);  // ESP32 RX, TX
  delay(1500);

  radar.print("AT+DEBUG=0\n");  // 기본 인원수 모드
  delay(300);
  radar.print("AT+START\n");
}

void loop() {
  while (radar.available()) {
    uint8_t b = radar.read();
    received++;

    if (used == 0 && b != 0x55) continue;
    if (used == 1 && b != 0xAA) {
      used = (b == 0x55) ? 1 : 0;
      continue;
    }

    frame[used++] = b;
    if (used < sizeof(frame)) continue;
    used = 0;

    uint8_t checksum = 0;
    for (int i = 2; i < 9; i++) checksum ^= frame[i];

    if (frame[2] == 0x0A && frame[3] == 0x04 && checksum == frame[9]) {
      validFrames++;
      Serial.printf("레이더 인원: %u명\n", (unsigned)frame[8]);
    }
  }

  if (millis() - lastReport >= 3000) {
    lastReport = millis();
    Serial.printf("[최근 3초] 수신 %lu바이트 / 정상 인원 데이터 %lu건\n",
                  (unsigned long)received, (unsigned long)validFrames);
    received = 0;
    validFrames = 0;
  }
}