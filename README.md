# 도와드림 진료실 앞 대기 인원 측정 장치 (ESP32-S3 + HLK-LD6001A)

진료실 문 위에 단 60GHz 레이더(HLK-LD6001A)로 대기 중인 사람의 위치와 인원을 재는 장치의 펌웨어와 확인용 화면입니다.

## 폴더

| 폴더 | 내용 |
|---|---|
| `firmware/radar_wifi/` | **현재 쓰는 펌웨어.** 센서 좌표 모드 + Wi-Fi. 보드가 실시간 화면과 데이터(`/data`)를 직접 제공 |
| `firmware/radar_position/` | Wi-Fi 없이 USB 시리얼로만 좌표를 내보내는 펌웨어 (PC 모니터용) |
| `firmware/count_mode/` | 처음 쓰던 인원수 전용 펌웨어 (센서 `AT+DEBUG=0`, 인원이 바뀔 때만 숫자 1개) |
| `monitor/` | 실시간 화면(`index.html`)과 USB 시리얼 → 웹 중계 서버(`server.ps1`, 설치 필요 없음) |

## 배선

| LD6001A | ESP32-S3 |
|---|---|
| TX | GPIO16 (UART1 RX) |
| RX | GPIO17 (UART1 TX) |
| GND | GND (공통) |
| VCC | 별도 전원 권장 (아래 전원 참고) |

센서 UART 115200bps 8N1.

## 전원 주의

Wi-Fi가 켜지는 순간 전류가 크게 튀어서, USB 허브 하나에 보드를 꽂으면 **Brownout(전압 부족)으로 계속 재부팅**됩니다.
보드의 USB 두 개를 PC(또는 충전기)에 따로 꽂거나, 센서를 별도 5V 전원에서 받고 센서 전원 단자 가까이에 470µF 커패시터를 달아 주세요.
보조배터리는 전류가 적으면 저절로 꺼지는 제품이 있으니 미리 시험해야 합니다.

## 업로드

Arduino IDE 또는 arduino-cli, 보드 `ESP32S3 Dev Module` (`esp32:esp32:esp32s3`), 출력은 CP210x 쪽 USB(예: COM8) 115200bps.

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32s3 firmware/radar_wifi
arduino-cli upload -p COM8 --fqbn esp32:esp32:esp32s3 firmware/radar_wifi
```

- `monitor/index.html`을 고쳤으면 `firmware/radar_wifi/make_page.ps1`을 실행해 `page.h`를 다시 만든 뒤 업로드합니다.
- 업로드 중 `Wrong boot mode detected`가 나오면 한 번 더 시도하거나, BOOT 버튼을 누른 채 RESET을 눌렀다 떼고 업로드합니다.
- 업로드 전에 `monitor/server.ps1`이나 IDE 시리얼 모니터가 포트를 잡고 있으면 닫아야 합니다.

## Wi-Fi 쓰는 법 (`radar_wifi`)

1. 처음 켜면 보드가 자기 Wi-Fi **Dowadream-Setup**(비밀번호 없음)을 엽니다.
2. 휴대폰으로 접속 → 자동으로 뜨는 화면(또는 `http://192.168.4.1/wifi`)에서 집 Wi-Fi·핫스팟을 고르고 비밀번호 입력.
   비밀번호는 보드 안(NVS)에만 저장되고 코드에는 들어가지 않습니다. 2.4GHz만 됩니다(아이폰 핫스팟은 "호환성 최대화" 켜기).
3. 연결되면 같은 Wi-Fi의 휴대폰/PC에서 `http://<보드 IP>` 로 실시간 화면을 봅니다. IP는 시리얼 출력 `# Wi-Fi 연결됨: http://...` 에 나옵니다.
4. 저장된 Wi-Fi를 1분 넘게 못 찾으면 보드가 다시 **Dowadream-Setup**을 열고, 거기 붙은 휴대폰에서 `http://192.168.4.1` 로 같은 화면을 볼 수 있습니다(인터넷 없어도 됨). 저장된 Wi-Fi는 2분마다 다시 찾습니다.
5. 시리얼로 `w` 를 보내면 저장된 Wi-Fi를 지우고 재시작, `r` 은 센서 재설정.

## PC에서 USB로 보기 (`monitor/`)

`monitor/start.bat` 더블클릭 → 크롬에서 `http://localhost:8765`. 포트가 COM8이 아니면 `start.bat`의 `COM8`을 바꿉니다.

## 시리얼/데이터 줄 형식

```
P,<x_mm>,<y_mm>,<z_mm>                         반사 점 1개 (프레임당 최대 25개)
T,<id>,<x_mm>,<y_mm>,<z_mm>,<vx_mm/s>,<vy_mm/s>  센서가 추적한 사람 1명
F,<frame>,<사람 수>,<점 수>,<millis>            프레임 끝 (약 10프레임/초)
S,<3초간 바이트>,<정상 프레임>,<버린 프레임>     상태
# ...                                          안내 메시지
```

이 센서는 앞쪽 거리를 **Z**로 줍니다(Y는 거의 0). 화면은 앞 거리 축을 자동으로 고릅니다.

## 센서 프로토콜 (AT+DEBUG=2)

`AT+STOP` → `AT+DEBUG=2` → `AT+START` 후 TLV 프레임:
`HEAD 01..08 | LENGTH u32 | FRAME u32 | TLV1=1 | POINTLEN | 점 25B×n | TLV2=2 | TRACKLEN | 사람 32B×n`
사람 32B = `Q u32, ID u32, X/Y/Z f32 (m), Vx/Vy/Vz f32 (m/s)`.
출처: 공식 문서가 아니라 공개 프로젝트 [thieryus007-cloud/HLK-6001A](https://github.com/thieryus007-cloud/HLK-6001A) 의 정리. 실제 장치에서 10fps, 오류 0으로 확인(2026-11-25).

## 화면의 데이터 정제 (`monitor/index.html`)

센서 자체 추적은 사람을 거의 내보내지 않고(30초 중 11프레임) 위치도 멈춰 있어서, 반사 점으로 직접 사람을 셉니다.

1. 0.3m보다 가깝거나 최대 거리(기본 4m)보다 먼 점, **빈 공간 보정** 때 잡힌 20cm 칸의 점은 버림
2. 1초 동안 모은 점을 DBSCAN으로 묶음 (기본 0.4m 안에 5개 이상 = 한 사람)
3. 0.8초 이상 보이면 사람으로 확정, 안 잡혀도 5초 유지, 위치는 부드럽게 이음
4. 대기 구역(X −2~2m, 앞 0.8~3m) 안 인원을 따로 셈

값은 화면 왼쪽 막대로 바꿀 수 있고 브라우저에 저장됩니다. 지금은 이 계산이 브라우저에서 돌고, 서버로 보내려면 펌웨어로 옮겨야 합니다.

## 남은 일

- 정제 계산을 펌웨어로 옮기기 (보드가 스스로 인원 판단)
- 1분 평균·최대 인원을 서버(Spring Boot on Render → Supabase)로 HTTPS 전송, 끊기면 쌓아 두었다 재전송
- 설치 높이·각도에 맞춘 좌표 변환과 기준값 확정 (시험 계획은 프로젝트 문서 참고)
