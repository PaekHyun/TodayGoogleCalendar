/*
 * Google Calendar → nanoESP32-C6 → WeAct 3.7" E-Paper
 * 오늘 일정을 4개 캘린더에서 가져와 e-paper에 표시
 * 
 * 하드웨어 연결 (nanoESP32-C6 → e-paper):
 *   GPIO4  (MOSI) → DIN (SDA)
 *   GPIO5  (SCK)  → CLK (SCK)
 *   GPIO7  (DC)   → DC
 *   GPIO0  (RST)  → RST (RES)
 *   GPIO1  (BUSY) → BUSY
 *   GPIO6  (CS)   → CS
 *   3V3            → VCC
 *   GND            → GND
 * 
 * 필요 라이브러리:
 *   - GxEPD2 (by Jean-Marc Capello)
 *   - Adafruit_GFX
 *   - U8g2_for_Adafruit_GFX (by oliver)  ← 한글 폰트 지원
 * 
 * 보드 설정: ESP32C6 Dev Module (esp32 by Espressif v2.x+)
 * 보드: nanoESP32-C6 (MuseLab, https://github.com/wuxx/nanoESP32-C6)
 */

#define ENABLE_GxEPD2_GFX 0

#include <SPI.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>  // 한글 폰트 지원

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ====== WiFi 설정 (secrets.h에서 수정) ======
#include "secrets.h"

// ====== 타임존 (KST = UTC+9) ======
const char* NTP_SERVER = "pool.ntp.org";
const long  TZ_OFFSET_SEC = 9 * 3600;   // UTC+9
const int   TZ_DST_SEC    = 0;

// ====== E-Paper 핀 설정 (nanoESP32-C6) ======
#define EPD_CS_PIN   6   // CS
#define EPD_DC_PIN   7   // DC
#define EPD_RST_PIN  0   // RST (RES)
#define EPD_BUSY_PIN 1   // BUSY
#define EPD_SCK_PIN  5   // SCK
#define EPD_MOSI_PIN 4   // MOSI (SDA/DIN)

#define VBAT_ADC_PIN        3      // 배터리 측정용 ADC 핀
#define VBAT_SAMPLES        8      // 평균낼 샘플 수
#define VBAT_DIVIDER_RATIO  2.0f   // 200kΩ : 200kΩ 분압


// [TestEPaper.ino 참고] 주석에 맞춰 핀 번호만 전달하여 객체 생성
GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT> display(
  GxEPD2_370_GDEY037T03(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN)
);

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts; // 한글 폰트 렌더러

// ====== 캘린더 설정 ======
struct CalendarInfo {
  const char* name;
  const char* url;
};

CalendarInfo calendars[] = {
  { CAL_NAME_1, CAL_URL_1 },
  { CAL_NAME_2, CAL_URL_2 },
  { CAL_NAME_3, CAL_URL_3 },
  { CAL_NAME_4, CAL_URL_4 },
};
const int NUM_CALENDARS = sizeof(calendars) / sizeof(calendars[0]);

// ====== 일정 구조체 ======
struct Event {
  String summary;
  String location;
  int    startHour;
  int    startMin;
  int    endHour;
  int    endMin;
  bool   allDay;
  int    calIndex;
};

const int MAX_EVENTS = 30;
Event events[MAX_EVENTS];
int    eventCount = 0;

// ====== 캘린더 마커 그리기 ======
void drawCalMarker(int x, int y, int calIndex) {
  switch (calIndex % 4) {
    case 0: display.fillRect(x, y - 8, 6, 8, GxEPD_BLACK); break;  // ■ 꽉 찬 사각형
    case 1: display.drawRect(x, y - 8, 6, 8, GxEPD_BLACK); break;  // □ 빈 사각형
    case 2: display.fillRect(x, y - 8, 6, 4, GxEPD_BLACK); break;  // ▀ 위쪽 반
    case 3: display.fillRect(x, y - 4, 6, 4, GxEPD_BLACK); break;  // ▄ 아래쪽 반
  }
}

// ====== 배터리 전압 계산 ======
// VBAT --[200kΩ]-- ADC_PIN --[200kΩ]-- GND 형태의 분배 회로를 가정.
// analogReadMilliVolts()는 ESP32 코어에 내장된 ADC 보정(esp_adc_cal)을 사용하므로
// analogRead() 원시값을 직접 계산하는 것보다 정확합니다.
float readBatteryVoltage() {
  uint32_t sumMilliVolts = 0;
  for (int i = 0; i < VBAT_SAMPLES; i++) {
    sumMilliVolts += analogReadMilliVolts(VBAT_ADC_PIN);
    delay(1000);
  }
  float avgMilliVolts = (float)sumMilliVolts / VBAT_SAMPLES;
  float vbat = (avgMilliVolts / 1000.0f) * VBAT_DIVIDER_RATIO;

  Serial.printf("VBAT ADC 평균: %.1fmV -> 배터리 전압: %.2fV\n", avgMilliVolts, vbat);
  return vbat;
}

// ====== ICS 파싱 유틸리티 ======
void parseDateTime(const String& dtStr, int& year, int& month, int& day, int& hour, int& minute, bool& isAllDay) {
  isAllDay = false;
  year = 0; month = 0; day = 0; hour = 0; minute = 0;

  // 공백 및 앞뒤 잡음(개행문자 \r 등) 철저히 제거
  String cleanStr = dtStr;
  cleanStr.trim();
  cleanStr.replace("\r", "");
  cleanStr.replace("\n", "");

  if (cleanStr.length() == 8) {
    // 종일 일정 (예: 20260714)
    isAllDay = true;
    year  = cleanStr.substring(0, 4).toInt();
    month = cleanStr.substring(4, 6).toInt();
    day   = cleanStr.substring(6, 8).toInt();
  } 
  else if (cleanStr.length() >= 15) {
    // 일반 일정 (예: 20250210T073000Z)
    int tempYear  = cleanStr.substring(0, 4).toInt();
    int tempMonth = cleanStr.substring(4, 6).toInt();
    int tempDay   = cleanStr.substring(6, 8).toInt();
    int tempHour  = cleanStr.substring(9, 11).toInt();
    int tempMin   = cleanStr.substring(11, 13).toInt();

    if (cleanStr.endsWith("Z")) {
      struct tm t = {0};
      t.tm_year = tempYear - 1900;
      t.tm_mon  = tempMonth - 1;
      t.tm_mday = tempDay;
      t.tm_hour = tempHour + 9; // UTC -> KST (+9시간)
      t.tm_min  = tempMin;

      mktime(&t); 

      year   = t.tm_year + 1900;
      month  = t.tm_mon + 1;
      day    = t.tm_mday;
      hour   = t.tm_hour;
      minute = t.tm_min;
    } else {
      year   = tempYear;
      month  = tempMonth;
      day    = tempDay;
      hour   = tempHour;
      minute = tempMin;
    }
  }
}

int todayYear, todayMonth, todayDay;

void getToday() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    todayYear  = 2026; todayMonth = 7; todayDay = 14; // 폴백
    return;
  }
  todayYear  = timeinfo.tm_year + 1900;
  todayMonth = timeinfo.tm_mon + 1;
  todayDay   = timeinfo.tm_mday;
}

void parseICS(const String& icsData, int calIndex) {
  int pos = 0;
  while (pos < (int)icsData.length() && eventCount < MAX_EVENTS) {
    int veventStart = icsData.indexOf("BEGIN:VEVENT", pos);
    if (veventStart < 0) break;
    int veventEnd = icsData.indexOf("END:VEVENT", veventStart);
    if (veventEnd < 0) break;

    String vevent = icsData.substring(veventStart, veventEnd);


    String summary = "";
    String location = ""; 
    String dtStart = "";
    String dtEnd = "";

    // VEVENT 내부를 줄 단위로 분할하여 파싱
    int linePos = 0;
    while (linePos < (int)vevent.length()) {
      int nextLine = vevent.indexOf('\n', linePos);
      if (nextLine < 0) nextLine = vevent.length();
      
      String line = vevent.substring(linePos, nextLine);
      line.trim();
      line.replace("\r", ""); // \r 잔여물 제거

      if (line.startsWith("SUMMARY:")) {
        summary = line.substring(8);
      } else if (line.startsWith("LOCATION:")) {
        location = line.substring(9);
      } else if (line.startsWith("DTSTART")) {
        int colonIdx = line.indexOf(':');
        if (colonIdx >= 0) dtStart = line.substring(colonIdx + 1);
      } else if (line.startsWith("DTEND")) {
        int colonIdx = line.indexOf(':');
        if (colonIdx >= 0) dtEnd = line.substring(colonIdx + 1);
      }

      linePos = nextLine + 1;
    }

    // 앞뒤 공백 최종 트리밍
    summary.trim();
    location.trim();
    dtStart.trim();
    dtEnd.trim();

    int sYear, sMonth, sDay, sHour, sMin;
    bool sAllDay;
    parseDateTime(dtStart, sYear, sMonth, sDay, sHour, sMin, sAllDay);

    int eYear, eMonth, eDay, eHour, eMin;
    bool eAllDay;
    parseDateTime(dtEnd, eYear, eMonth, eDay, eHour, eMin, eAllDay);

    // [일정 판별 및 매칭]
    bool isToday = false;
    
    if (sAllDay) {
      if (sYear == todayYear && sMonth == todayMonth && sDay == todayDay) {
        isToday = true;
      }
      else if (sYear <= todayYear && eYear >= todayYear) {
        int todayScore = todayYear * 10000 + todayMonth * 100 + todayDay;
        int startScore = sYear * 10000 + sMonth * 100 + sDay;
        int endScore   = eYear * 10000 + eMonth * 100 + eDay;
        
        if (todayScore >= startScore && todayScore < endScore) {
          isToday = true;
        }
      }
    } else {
      if (sYear == todayYear && sMonth == todayMonth && sDay == todayDay) {
        isToday = true;
      }
    }

    if (isToday && summary.length() > 0) {
      events[eventCount].summary   = summary;
      events[eventCount].location  = location;
      events[eventCount].startHour = sHour;
      events[eventCount].startMin  = sMin;
      events[eventCount].endHour   = eHour;
      events[eventCount].endMin    = eMin;
      events[eventCount].allDay    = sAllDay;
      events[eventCount].calIndex  = calIndex;
      
      Serial.printf("  >> [추출성공] 캘린더[%d] | 일정: %s | 시작: %02d:%02d (종일: %s)\n", 
                    calIndex, summary.c_str(), sHour, sMin, sAllDay ? "예" : "아니오");

      eventCount++;

      delay(1000);
    } else {
      // 파싱 시도했으나 오늘 일정이 아닌 경우 디버그용 출력 (필요 시 주석 해제)
      // Serial.printf("  >> [패스] 일정: %s | 날짜: %04d-%02d-%02d\n", summary.c_str(), sYear, sMonth, sDay);
    }

    pos = veventEnd + 10;
  }
}

void sortEvents() {
  for (int i = 0; i < eventCount - 1; i++) {
    for (int j = i + 1; j < eventCount; j++) {
      int timeI = events[i].allDay ? -1 : events[i].startHour * 60 + events[i].startMin;
      int timeJ = events[j].allDay ? -1 : events[j].startHour * 60 + events[j].startMin;
      if (timeI > timeJ) {
        Event tmp = events[i];
        events[i] = events[j];
        events[j] = tmp;
      }
    }
  }
}



String fetchICS(const char* url) {
  HTTPClient http;
  String payload = "";

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(5000);
  http.setConnectTimeout(3000);

  if (http.begin(url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      payload = http.getString();
    } else {
      Serial.printf("[HTTP] GET → code: %d\n", httpCode);
    }
    http.end();
  } else {
    Serial.println("[HTTP] begin failed");
  }
  return payload;
}




// UTF-8 문자 경계를 지키면서, 주어진 픽셀 폭 안에 들어가도록 자르는 함수
String truncateToWidth(U8G2_FOR_ADAFRUIT_GFX& fonts, const String& text, int maxWidthPx) {
  if (fonts.getUTF8Width(text.c_str()) <= maxWidthPx) {
    return text;  // 안 잘라도 됨
  }

  String result = "";
  int i = 0;
  int len = text.length();

  while (i < len) {
    // 현재 문자의 바이트 길이 계산 (UTF-8 선두 바이트로 판별)
    unsigned char c = (unsigned char)text[i];
    int charLen = 1;
    if ((c & 0x80) == 0x00) charLen = 1;       // 0xxxxxxx: ASCII
    else if ((c & 0xE0) == 0xC0) charLen = 2;  // 110xxxxx
    else if ((c & 0xF0) == 0xE0) charLen = 3;  // 1110xxxx: 한글 대부분 여기
    else if ((c & 0xF8) == 0xF0) charLen = 4;  // 11110xxx

    if (i + charLen > len) break; // 깨진 데이터 방지

    String candidate = result + text.substring(i, i + charLen) + "..";
    if (fonts.getUTF8Width(candidate.c_str()) > maxWidthPx) {
      break; // "..." 붙였을 때 넘치면 여기서 멈춤
    }

    result += text.substring(i, i + charLen);
    i += charLen;
  }

  return result + "..";
}








// ====== E-Paper 표시 ======
void displayCalendar() {
  display.setRotation(1); // 가로 모드: 416 x 240
  display.setTextColor(GxEPD_BLACK);

  const int W = display.width();   // 416
  const int H = display.height(); // 240

  float vbatVoltage = readBatteryVoltage();

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // ---- 헤더: 날짜 ----
    char dateStr[40];
    const char* dayNames[] = {"일", "월", "화", "수", "목", "금", "토"};
    struct tm timeinfo;
    getLocalTime(&timeinfo);
    snprintf(dateStr, sizeof(dateStr), "%d년 %d월 %d일 (%s)",
             todayYear, todayMonth, todayDay, dayNames[timeinfo.tm_wday]);

    u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
    u8g2Fonts.setCursor(8, 16);
    u8g2Fonts.print(dateStr);

    display.drawLine(0, 22, W, 22, GxEPD_BLACK);

    // ---- 일정 목록 ----
    int y = 38;
    const int lineHeight = 20;
    const int maxVisible = 8;

    if (eventCount == 0) {
      u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
      u8g2Fonts.setCursor(20, y + 10);
      u8g2Fonts.print("오늘 일정이 없습니다");
    } else {
      int showCount = min(eventCount, maxVisible);
      for (int i = 0; i < showCount; i++) {
        Event& ev = events[i];

        char timeBuf[20];
        if (ev.allDay) {
          snprintf(timeBuf, sizeof(timeBuf), "종일");
        } else {
          snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d-%02d:%02d",
                   ev.startHour, ev.startMin, ev.endHour, ev.endMin);
        }


        u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);

        // 시간 출력
        const int timeX = 4;
        u8g2Fonts.setCursor(timeX, y);
        u8g2Fonts.print(timeBuf);

        // 시간 문자열의 실제 폭 계산
        int timeWidth = u8g2Fonts.getUTF8Width(timeBuf);

        // 시간 뒤에 8픽셀 띄우고 마커 배치
        int markerX = timeX + timeWidth + 8;
        drawCalMarker(markerX, y, ev.calIndex);

        // 마커 뒤에 10픽셀 띄우고 제목 출력
        u8g2Fonts.setCursor(markerX + 10, y);

        int availWidth = W - (markerX + 10) - 4;
        String title = truncateToWidth(u8g2Fonts, ev.summary, availWidth);


        u8g2Fonts.print(title);



        if (ev.location.length() > 0 && y + 14 < H - 20) {
          u8g2Fonts.setCursor(92, y + 14);
          int availWidthLoc = W - 92 - 4;
          String loc = truncateToWidth(u8g2Fonts, ev.location, availWidthLoc);
          u8g2Fonts.print(loc);
          y += lineHeight + 14;
        } else {
          y += lineHeight;
        }

        if (i < showCount - 1 && y < H - 20) {
          // display.drawFastHLine(4, y - 4, W - 8, GxEPD_BLACK);
          display.drawFastHLine(4, y + 5, W - 8, GxEPD_BLACK);
        }
      }

      if (eventCount > maxVisible) {
        u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
        u8g2Fonts.setCursor(W - 80, H - 6);
        char moreBuf[20];
        snprintf(moreBuf, sizeof(moreBuf), "+%d개 더", eventCount - maxVisible);
        u8g2Fonts.print(moreBuf);
      }
    }

    // ---- 하단: 업데이트 시간 ----
    u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
    char updateStr[30];
    snprintf(updateStr, sizeof(updateStr), "%02d:%02d 갱신",
             timeinfo.tm_hour, timeinfo.tm_min);
    u8g2Fonts.setCursor(4, H - 4);
    u8g2Fonts.print(updateStr);

    // ---- 하단: 캘린더 범례 (배터리 전압 표시를 위해 한 줄 위로 이동) ----
    int legendX = W - 40 * NUM_CALENDARS;
    int legendY = H - 20;
    for (int i = 0; i < NUM_CALENDARS; i++) {
      drawCalMarker(legendX, legendY, i);
      u8g2Fonts.setCursor(legendX + 8, legendY);
      u8g2Fonts.print(calendars[i].name);
      legendX += 40;
    }

    // ---- 하단 오른쪽: 배터리 전압 ----
    char battBuf[16];
    snprintf(battBuf, sizeof(battBuf), "%.2fV", vbatVoltage);
    int battWidth = u8g2Fonts.getUTF8Width(battBuf);
    u8g2Fonts.setCursor(W - battWidth - 4, H - 4);
    u8g2Fonts.print(battBuf);

  } while (display.nextPage());
}

// ====== 에러 표시 ======
void displayError(const char* msg) {
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFont(u8g2_font_unifont_t_korean2);
    u8g2Fonts.setCursor(20, 60);
    u8g2Fonts.print("오류");
    u8g2Fonts.setCursor(20, 85);
    u8g2Fonts.print(msg);
  } while (display.nextPage());
}

// ====== WiFi 연결 ======
bool connectWiFi() {
  Serial.printf("WiFi 연결 중: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi 연결됨!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("WiFi 연결 실패!");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return false;
  }
}

// ====== NTP 시간 동기화 ======
bool syncTime() {
  configTime(TZ_OFFSET_SEC, TZ_DST_SEC, NTP_SERVER);
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 20) {
    delay(500);
    attempts++;
  }
  if (attempts >= 20) {
    Serial.println("NTP 시간 동기화 실패!");
    return false;
  }
  Serial.println("NTP 시간 동기화 완료");
  return true;
}

void powerOffWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi OFF (절전)");
}

void powerOffEPaper() {
  pinMode(EPD_RST_PIN, OUTPUT);
  digitalWrite(EPD_RST_PIN, LOW);
  Serial.println("E-Paper 전원 차단 (절전)");
}

// ====== 딥슬립 진입 ======
void enterDeepSleep() {
  Serial.printf("딥슬립 진입 (%d분 후 갱신)\n", REFRESH_INTERVAL_MIN);

  powerOffWiFi();
  powerOffEPaper();

  pinMode(EPD_MOSI_PIN, INPUT_PULLDOWN);  // MOSI (GPIO4)
  pinMode(EPD_SCK_PIN, INPUT_PULLDOWN);   // SCK  (GPIO5)
  pinMode(EPD_CS_PIN, INPUT_PULLDOWN);
  pinMode(EPD_DC_PIN, INPUT_PULLDOWN);
  pinMode(EPD_BUSY_PIN, INPUT_PULLDOWN);

  esp_sleep_enable_timer_wakeup((uint64_t)REFRESH_INTERVAL_MIN * 60 * 1000000ULL);
  esp_deep_sleep_start();
}


void processEvent(const String& summary, const String& location,
                   const String& dtStart, const String& dtEnd, int calIndex) {
  if (eventCount >= MAX_EVENTS) return;

  int sYear, sMonth, sDay, sHour, sMin;
  bool sAllDay;
  parseDateTime(dtStart, sYear, sMonth, sDay, sHour, sMin, sAllDay);

  int eYear, eMonth, eDay, eHour, eMin;
  bool eAllDay;
  parseDateTime(dtEnd, eYear, eMonth, eDay, eHour, eMin, eAllDay);

  bool isToday = false;
  if (sAllDay) {
    if (sYear == todayYear && sMonth == todayMonth && sDay == todayDay) {
      isToday = true;
    } else if (sYear <= todayYear && eYear >= todayYear) {
      int todayScore = todayYear * 10000 + todayMonth * 100 + todayDay;
      int startScore = sYear * 10000 + sMonth * 100 + sDay;
      int endScore   = eYear * 10000 + eMonth * 100 + eDay;
      if (todayScore >= startScore && todayScore < endScore) isToday = true;
    }
  } else {
    if (sYear == todayYear && sMonth == todayMonth && sDay == todayDay) isToday = true;
  }

  if (isToday && summary.length() > 0) {
    events[eventCount].summary   = summary;
    events[eventCount].location  = location;
    events[eventCount].startHour = sHour;
    events[eventCount].startMin  = sMin;
    events[eventCount].endHour   = eHour;
    events[eventCount].endMin    = eMin;
    events[eventCount].allDay    = sAllDay;
    events[eventCount].calIndex  = calIndex;

    Serial.printf("  >> [추출성공] 캘린더[%d] | 일정: %s | 시작: %02d:%02d (종일: %s)\n",
                  calIndex, summary.c_str(), sHour, sMin, sAllDay ? "예" : "아니오");
    eventCount++;
  }
}

// String 통째로 만들지 않고, 스트림에서 한 줄씩 읽어 바로 파싱
bool fetchAndParseICS(const char* url, int calIndex) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(8000);
  http.setConnectTimeout(3000);

  if (!http.begin(url)) {
    Serial.println("[HTTP] begin failed");
    return false;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("[HTTP] GET -> code: %d\n", httpCode);
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();

  bool inEvent = false, sawEndCalendar = false;
  size_t totalBytes = 0;
  String summary, location, dtStart, dtEnd;

  unsigned long lastData = millis();
  while (http.connected() && (millis() - lastData < 8000)) {
    if (!stream->available()) { delay(2); continue; }

    String line = stream->readStringUntil('\n');
    lastData = millis();
    totalBytes += line.length();
    line.trim();
    line.replace("\r", "");

    if (line.startsWith("BEGIN:VEVENT")) {
      inEvent = true;
      summary = ""; location = ""; dtStart = ""; dtEnd = "";
    } else if (line.startsWith("END:VEVENT")) {
      if (inEvent) processEvent(summary, location, dtStart, dtEnd, calIndex);
      inEvent = false;
    } else if (inEvent) {
      if (line.startsWith("SUMMARY:")) summary = line.substring(8);
      else if (line.startsWith("LOCATION:")) location = line.substring(9);
      else if (line.startsWith("DTSTART")) {
        int c = line.indexOf(':'); if (c >= 0) dtStart = line.substring(c + 1);
      } else if (line.startsWith("DTEND")) {
        int c = line.indexOf(':'); if (c >= 0) dtEnd = line.substring(c + 1);
      }
    } else if (line.startsWith("END:VCALENDAR")) {
      sawEndCalendar = true;
    }
  }

  http.end();
  Serial.printf("  → %u bytes 스트리밍 처리 (여유힙: %u)\n", totalBytes, ESP.getFreeHeap());

  if (!sawEndCalendar) {
    Serial.println("  → END:VCALENDAR 못 봄 (스트림 끊김/타임아웃)");
    return false;
  }
  return true;
}



// ====== 메인 일정 갱신 ======
void refreshCalendar() {
  eventCount = 0;
  getToday();

  Serial.printf("오늘: %d-%02d-%02d\n", todayYear, todayMonth, todayDay);

  int failCount = 0;

  /*
  for (int i = 0; i < NUM_CALENDARS; i++) {
    Serial.printf("캘린더 '%s' 다운로드 중...\n", calendars[i].name);
    String icsData = fetchICS(calendars[i].url);
    if (icsData.length() > 0) {
      Serial.printf("  → %d bytes 수신\n", icsData.length());
      parseICS(icsData, i);
    } else {
      Serial.printf("  → 수신 실패\n");
      failCount++;
    }
  }
*/
  for (int i = 0; i < NUM_CALENDARS; i++) {
  Serial.printf("캘린더 '%s' 다운로드 중... (여유힙: %u)\n", calendars[i].name, ESP.getFreeHeap());
  if (!fetchAndParseICS(calendars[i].url, i)) {
    Serial.printf("  → 수신 실패\n");
    failCount++;
    }
  }


  if (failCount == NUM_CALENDARS) {
    Serial.println("모든 캘린더 다운로드 실패");
  }

  Serial.printf("총 %d개 일정\n", eventCount);
  sortEvents();
  displayCalendar();
}







// ====== Arduino setup/loop ======
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); }
  Serial.println("\n=== Google Calendar E-Paper Display ===");

  // 배터리 전압 측정용 ADC 설정 (12비트, 0~3.3V 풀레인지)
  analogReadResolution(12);
  analogSetPinAttenuation(VBAT_ADC_PIN, ADC_11db);

  // [수정 핵심] TestEPaper.ino 처럼 SPI.begin() 호출을 제거합니다.
  // GxEPD2 내부에서 기본 SPI 핀(SCK=19, MOSI=18)을 통해 자동 초기화됩니다.
  SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, EPD_CS_PIN);
  // [TestEPaper.ino 참고] 디스플레이 초기화
  display.init(115200, true, 5, false);

  // 한글 폰트 초기화
  u8g2Fonts.begin(display);
  u8g2Fonts.setForegroundColor(GxEPD_BLACK);
  u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
  u8g2Fonts.setFontMode(1);       
  u8g2Fonts.setFontDirection(0);  

  // WiFi 연결
  if (!connectWiFi()) {
    displayError("WiFi 연결 실패\n30초 후 재시도");
    display.hibernate();
    delay(30000);
    ESP.restart();
  }

  // NTP 시간 동기화
  if (!syncTime()) {
    displayError("시간 동기화 실패\n30초 후 재시도");
    display.hibernate();
    delay(30000);
    ESP.restart();
  }

  // 일정 가져와서 표시
  refreshCalendar();
  
  // 디스플레이 절전 및 딥슬립
  display.hibernate();
  enterDeepSleep();
  
}

void loop() {
  // 딥슬립 사용으로 루프는 타지 않음
}
