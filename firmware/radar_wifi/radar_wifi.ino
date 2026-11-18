/*
  도와드림 LD6001A Wi-Fi 펌웨어 (ESP32-S3)
  ------------------------------------------
  radar_position.ino 기능 + Wi-Fi.
  - 처음 켜거나 저장된 Wi-Fi에 못 붙으면 설정용 Wi-Fi "Dowadream-Setup" 을 엽니다.
    휴대폰으로 거기에 접속 -> 자동으로 뜨는 화면(안 뜨면 http://192.168.4.1)에서 집 Wi-Fi를 고르고 비밀번호 입력.
    비밀번호는 보드 안(NVS)에만 저장됩니다.
  - 연결되면 같은 Wi-Fi의 휴대폰/노트북에서 http://dowadream.local 또는 http://<IP> 로 실시간 화면을 봅니다.
  - USB 시리얼(115200)로도 같은 줄이 계속 나옵니다. 시리얼에 'w' 를 보내면 Wi-Fi 설정을 지우고 재시작.

  줄 형식: P,x,y,z(mm) / T,id,x,y,z,vx,vy / F,frame,사람수,점수,millis / S,바이트,정상,버림 / # 안내
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include "page.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
// 시험용: USB 전원이 약해 Wi-Fi 켤 때 전압 감시(Brownout)가 보드를 재시작시킴 -> 임시로 끔.
// 전원 보강(센서 별도 전원, 470uF 커패시터) 후에는 이 줄을 false로 바꿔 다시 켜기.
const bool DISABLE_BROWNOUT = false;

// ---------------- 레이더 ----------------
HardwareSerial radar(1);
#define RADAR_RX 16
#define RADAR_TX 17
const uint32_t RADAR_BAUD = 115200;
const int MAX_POINTS = 25;
static uint8_t buf[6144];
static size_t blen = 0;
uint32_t rxBytes = 0, okFrames = 0, badFrames = 0, lastStat = 0, lastOk = 0;
static const uint8_t HEAD[8] = {1, 2, 3, 4, 5, 6, 7, 8};

// ---------------- Wi-Fi / 웹 ----------------
WebServer server(80);
DNSServer dns;
Preferences prefs;
bool portalMode = false;
uint32_t portalSince = 0, lastIpMsg = 0;
// Wi-Fi 송신 세기. 켤 때 순간 전류가 커서 USB 전원이 약하면 전압이 떨어져 재부팅(Brownout)됨 -> 낮춰서 시작
const wifi_power_t TX_POWER = WIFI_POWER_8_5dBm;

// 최근 줄 보관 (웹페이지가 /data?since=N 으로 가져감)
const int RING = 1500;
String ring[RING];
uint32_t seqN = 0;

void emit(const String &s) {
  Serial.println(s);
  ring[seqN % RING] = s;
  seqN++;
}
void emitf(const char *fmt, ...) {
  char tmp[160];
  va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
  emit(String(tmp));
}

uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
float f32(const uint8_t *p) { float f; memcpy(&f, p, 4); return f; }
int mm(float m) { return isfinite(m) ? (int)lroundf(m * 1000.0f) : 0; }

void startRadar() {
  emit("# 센서 설정: AT+STOP -> AT+DEBUG=2 -> AT+START");
  radar.print("AT+STOP\n");    delay(300);
  radar.print("AT+DEBUG=2\n"); delay(300);
  radar.print("AT+START\n");   delay(300);
}

void dropFront(size_t n) {
  if (n >= blen) { blen = 0; return; }
  memmove(buf, buf + n, blen - n);
  blen -= n;
}

bool parseOne() {
  size_t h = 0;
  while (h + 8 <= blen && memcmp(buf + h, HEAD, 8) != 0) h++;
  if (h + 8 > blen) { if (blen > 7) dropFront(blen - 7); return false; }
  if (h) dropFront(h);
  if (blen < 32) return false;
  uint32_t len = u32(buf + 8), frame = u32(buf + 12);
  if (len < 32 || len > sizeof(buf)) { badFrames++; dropFront(1); return true; }
  if (blen < len) return false;
  uint32_t tlv1 = u32(buf + 16), plen = u32(buf + 20), tlen = 0;
  bool ok = tlv1 == 1 && plen % 25 == 0 && 24 + plen + 8 <= len;
  if (ok) { uint32_t tlv2 = u32(buf + 24 + plen); tlen = u32(buf + 28 + plen);
            ok = tlv2 == 2 && tlen % 32 == 0 && 24 + plen + 8 + tlen == len; }
  if (!ok) { badFrames++; dropFront(1); return true; }

  int np = plen / 25, stepP = np > MAX_POINTS ? (np + MAX_POINTS - 1) / MAX_POINTS : 1;
  for (int i = 0; i < np; i += stepP) {
    const uint8_t *p = buf + 24 + i * 25;
    emitf("P,%d,%d,%d", mm(f32(p)), mm(f32(p + 4)), mm(f32(p + 8)));
  }
  int nt = tlen / 32;
  const uint8_t *t0 = buf + 32 + plen;
  for (int i = 0; i < nt; i++) {
    const uint8_t *t = t0 + i * 32;
    emitf("T,%lu,%d,%d,%d,%d,%d", (unsigned long)u32(t + 4), mm(f32(t + 8)), mm(f32(t + 12)),
          mm(f32(t + 16)), mm(f32(t + 20)), mm(f32(t + 24)));
  }
  emitf("F,%lu,%d,%d,%lu", (unsigned long)frame, nt, np, (unsigned long)millis());
  okFrames++; lastOk = millis();
  dropFront(len);
  return true;
}

// ---------------- 웹 핸들러 ----------------
void jsonEsc(String &o, const String &s) {
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) o += ' ';
    else o += c;
  }
}

void handleData() {
  uint32_t since = server.arg("since").toInt();
  uint32_t oldest = seqN > RING ? seqN - RING : 0;     // 남아 있는 가장 오래된 seq-1
  if (since < oldest) since = oldest;
  if (since > seqN) since = oldest;    // 보드 재시작 후 페이지가 큰 번호를 들고 있을 때
  String out; out.reserve(24000);
  out += "{\"port\":\"Wi-Fi\",\"status\":\"open\",\"seq\":"; out += seqN; out += ",\"lines\":[";
  int cnt = 0;
  for (uint32_t s = since + 1; s <= seqN && cnt < 700; s++, cnt++) {
    if (cnt) out += ',';
    out += "{\"seq\":"; out += s; out += ",\"t\":0,\"kind\":\"serial\",\"line\":\"";
    jsonEsc(out, ring[(s - 1) % RING]);
    out += "\"}";
  }
  out += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", out);
}

String setupPage(const String &msg) {
  int n = WiFi.scanNetworks();
  String h = F("<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>도와드림 Wi-Fi 설정</title><style>body{font:16px system-ui;margin:24px;max-width:420px}"
               "select,input,button{font:inherit;width:100%;padding:10px;margin:6px 0;box-sizing:border-box}"
               "button{background:#2f6fde;color:#fff;border:0;border-radius:8px}</style>"
               "<h2>도와드림 장치 Wi-Fi 설정</h2>");
  if (msg.length()) h += "<p style='color:#d64545'>" + msg + "</p>";
  h += F("<form method=post action=/save><label>Wi-Fi 이름</label><select name=ssid>");
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i); if (!s.length()) continue;
    String e = s; e.replace("'", "&#39;"); e.replace("<", "&lt;");
    h += "<option value='" + e + "'>" + e + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
  }
  h += F("</select><label>목록에 없으면 직접 입력</label><input name=ssid2 placeholder='Wi-Fi 이름'>"
         "<label>비밀번호</label><input name=pass type=password>"
         "<button>저장하고 연결</button></form><p style='color:#666;font-size:14px'>2.4GHz Wi-Fi만 됩니다. 저장하면 장치가 재시작합니다.</p>");
  return h;
}

void handleSave() {
  String ssid = server.arg("ssid2").length() ? server.arg("ssid2") : server.arg("ssid");
  prefs.putString("ssid", ssid);
  prefs.putString("pass", server.arg("pass"));
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><h2>저장했습니다</h2><p>장치가 재시작해서 '" + ssid + "'에 연결합니다. 휴대폰을 같은 Wi-Fi로 돌려놓고 http://dowadream.local 을 열어 보세요. 연결이 안 되면 이 보드 Wi-Fi(Dowadream-Setup)가 다시 열리고, http://192.168.4.1 에서 화면을 볼 수 있습니다.</p>");
  delay(1500);
  ESP.restart();
}

void setupRoutes() {
  server.on("/data", handleData);
  server.on("/reset", HTTP_POST, []() { server.send(200, "application/json", "{\"ok\":true}"); delay(300); ESP.restart(); });
  server.on("/save", HTTP_POST, handleSave);
  server.on("/wifi", []() { server.send(200, "text/html; charset=utf-8", setupPage("")); });
  server.on("/", []() { server.send_P(200, "text/html; charset=utf-8", PAGE_HTML); });   // 실시간 화면 (인터넷 없어도 됨)
  server.onNotFound([]() {   // 보드 자체 Wi-Fi에 붙은 휴대폰의 자동 팝업: Wi-Fi 저장 전엔 설정, 후엔 실시간 화면
    if (portalMode) {
      server.sendHeader("Location", prefs.getString("ssid", "").length() ? "http://192.168.4.1/" : "http://192.168.4.1/wifi");
      server.send(302, "text/plain", "");
    } else server.send(404, "text/plain", "not found");
  });
}

void startPortal(const char *why) {
  portalMode = true; portalSince = millis();
  WiFi.mode(WIFI_AP_STA);
  WiFi.setTxPower(TX_POWER);
  WiFi.softAP("Dowadream-Setup");
  dns.start(53, "*", WiFi.softAPIP());
  emitf("# %s -> 보드 자체 Wi-Fi 'Dowadream-Setup' 열림. 휴대폰으로 접속 후 실시간 화면 http://192.168.4.1 , Wi-Fi 설정 http://192.168.4.1/wifi", why);
}

void onConnected();
void connectWifi() {
  String ssid = prefs.getString("ssid", ""), pass = prefs.getString("pass", "");
  if (!ssid.length()) { startPortal("저장된 Wi-Fi 없음"); return; }
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(TX_POWER);
  WiFi.setHostname("dowadream");
  WiFi.begin(ssid.c_str(), pass.c_str());
  emitf("# Wi-Fi '%s' 연결 중...", ssid.c_str());
  uint32_t t = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t < 15000) { delay(200); }
  if (WiFi.status() != WL_CONNECTED) { startPortal("Wi-Fi 연결 실패"); return; }
  onConnected();
}

bool mdnsOn = false;
uint32_t lastRetry = 0, lostSince = 0;
void onConnected() {
  WiFi.setAutoReconnect(true);
  if (!mdnsOn) { MDNS.begin("dowadream"); MDNS.addService("http", "tcp", 80); mdnsOn = true; }
  emitf("# Wi-Fi 연결됨: http://%s  (또는 http://dowadream.local)", WiFi.localIP().toString().c_str());
}

// 인터넷/공유기가 없어도 계속 쓰기: 연결이 1분 넘게 끊기면 보드 자체 Wi-Fi를 열고, 저장된 Wi-Fi는 2분마다 다시 찾음
void keepWifi() {
  bool up = WiFi.status() == WL_CONNECTED;
  static bool wasUp = false;
  if (up && !wasUp && portalMode) onConnected();
  wasUp = up;
  if (up) { lostSince = 0; return; }
  if (!lostSince) lostSince = millis();
  if (!portalMode && millis() - lostSince > 60000) startPortal("Wi-Fi 1분 넘게 끊김");
  String ssid = prefs.getString("ssid", "");
  if (portalMode && ssid.length() && millis() - lastRetry > 120000) {
    lastRetry = millis();
    emitf("# 저장된 Wi-Fi '%s' 다시 찾는 중", ssid.c_str());
    WiFi.begin(ssid.c_str(), prefs.getString("pass", "").c_str());
  }
}

void setup() {
  if (DISABLE_BROWNOUT) WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  Serial.begin(115200);
  radar.setRxBufferSize(8192);
  radar.begin(RADAR_BAUD, SERIAL_8N1, RADAR_RX, RADAR_TX);
  prefs.begin("wifi", false);
  delay(800);
  emit("# 도와드림 LD6001A Wi-Fi 펌웨어");
  setupRoutes();
  startRadar();
  delay(500);
  connectWifi();
  server.begin();
  lastOk = millis();
}

void loop() {
  while (radar.available()) {
    int c = radar.read(); rxBytes++;
    if (blen < sizeof(buf)) buf[blen++] = (uint8_t)c; else { dropFront(sizeof(buf) / 2); badFrames++; }
  }
  while (parseOne()) {}
  server.handleClient();
  if (portalMode) {
    dns.processNextRequest();
  }
  keepWifi();

  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'r') startRadar();
    if (c == 'w') { prefs.clear(); emit("# Wi-Fi 설정 지움 -> 재시작"); delay(300); ESP.restart(); }
  }

  if (millis() - lastStat >= 3000) {
    lastStat = millis();
    emitf("S,%lu,%lu,%lu", (unsigned long)rxBytes, (unsigned long)okFrames, (unsigned long)badFrames);
    if (rxBytes > 0 && okFrames == 0 && blen >= 16) {
      String s = "# 해석 못 한 데이터 앞부분:"; char hx[4];
      for (size_t i = 0; i < min(blen, (size_t)32); i++) { snprintf(hx, 4, " %02X", buf[i]); s += hx; }
      emit(s);
    }
    rxBytes = okFrames = badFrames = 0;
    if (millis() - lastOk > 10000) { emit("# 10초간 프레임 없음 -> 센서 다시 설정"); startRadar(); lastOk = millis(); }
  }
  if (millis() - lastIpMsg >= 30000) {
    lastIpMsg = millis();
    if (WiFi.status() == WL_CONNECTED && portalMode) emitf("# Wi-Fi 연결 유지: http://%s  (보드 자체 Wi-Fi도 켜져 있음: 192.168.4.1)", WiFi.localIP().toString().c_str());
    else if (portalMode) emit("# 보드 자체 Wi-Fi 모드: 휴대폰으로 'Dowadream-Setup' 접속 -> 실시간 화면 http://192.168.4.1 , 설정 /wifi");
    else if (WiFi.status() == WL_CONNECTED) emitf("# Wi-Fi 연결 유지: http://%s  신호 %d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else emit("# Wi-Fi 끊김, 다시 연결 중");
  }
}
