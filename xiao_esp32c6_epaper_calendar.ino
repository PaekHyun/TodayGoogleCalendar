/*
 * Google Calendar → XIAO ESP32C6 → WeAct 3.7" E-Paper
 * 오늘 일정을 4개 캘린더에서 가져와 e-paper에 표시
 * 
 * 하드웨어 연결 (XIAO ESP32C6 → e-paper):
 *   D10 (MOSI/GPIO18) → DIN (SDA)
 *   D8  (SCK/GPIO19)  → CLK (SCK)
 *   D1  (GPIO1)        → DC
 *   D2  (GPIO2)        → RST (RES)
 *   D3  (GPIO3)        → BUSY
 *   D0  (GPIO0/CS)     → CS
 *   3V3                → VCC
 *   GND                → GND
 * 
 * 필요 라이브러리:
 *   - GxEPD2 (by Jean-Marc Capello)
 *   - Adafruit_GFX
 * 
 * 보드 설정: XIAO_ESP32C6 (esp32 by Espressif v2.x+)
 */

#define ENABLE_GxEPD2_GFX 0

#include <GxEPD2_BW.h>
#include <GxEPD2_370_GDEY037T03.h> // 3.7" 240x416 UC8253
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans12pt7b.h>

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ====== WiFi 설정 (secrets.h에서 수정) ======
#include "secrets.h"

// ====== 타임존 (KST = UTC+9) ======
const char* NTP_SERVER = "pool.ntp.org";
const long  TZ_OFFSET_SEC = 9 * 3600;   // UTC+9
const int   TZ_DST_SEC    = 0;

// ====== E-Paper 핀 설정 (XIAO ESP32C6) ======
#define EPD_CS_PIN   0   // D0 / GPIO0
#define EPD_DC_PIN   1   // D1 / GPIO1
#define EPD_RST_PIN  2   // D2 / GPIO2
#define EPD_BUSY_PIN 3   // D3 / GPIO3
// SPI: D8=SCK(19), D10=MOSI(18)

GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT> display(
  GxEPD2_370_GDEY037T03(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN)
);

// ====== Google Calendar ICS URL ======
struct CalendarInfo {
  const char* name;
  const char* url;
};

CalendarInfo calendars[] = {
  { "calendar1", "https://xxxxx.xxx/calendar1/basic.ics" },
  { "calendar2", "https://xxxxx.xxx/calendar2/basic.ics" },
  { "calendar3", "https://xxxxx.xxx/calendar3/basic.ics" },
  { "calendar4", "https://xxxxx.xxx/calendar4/basic.ics" },
};
const int NUM_CALENDARS = 4;

// ====== 일정 구조체 ======
struct Event {
  String summary;
  String location;
  int    startHour;
  int    startMin;
  int    endHour;
  int    endMin;
  bool   allDay;
  String calName;  // 어느 캘린더인지
};

const int MAX_EVENTS = 30;
Event events[MAX_EVENTS];
int    eventCount = 0;

// ====== ICS 파싱 유틸리티 ======

// ICS의 DTSTART/DTEND 파싱
// 형식: "20260713T090000" (로컬) 또는 "20260713T090000Z" (UTC) 또는 "20260713" (종일)
void parseDateTime(const String& dtStr, int& year, int& month, int& day, int& hour, int& minute, bool& isAllDay) {
  isAllDay = false;
  year  = 0; month = 0; day = 0; hour = 0; minute = 0;

  if (dtStr.length() == 8) {
    // 종일: 20260713
    isAllDay = true;
    year  = dtStr.substring(0, 4).toInt();
    month = dtStr.substring(4, 6).toInt();
    day   = dtStr.substring(6, 8).toInt();
  } else if (dtStr.length() >= 15) {
    year  = dtStr.substring(0, 4).toInt();
    month = dtStr.substring(4, 6).toInt();
    day   = dtStr.substring(6, 8).toInt();
    hour  = dtStr.substring(9, 11).toInt();
    minute = dtStr.substring(11, 13).toInt();

    // Z结尾면 UTC → KST 변환
    if (dtStr.endsWith("Z")) {
      // UTC → KST (+9)
      struct tm t = {0};
      t.tm_year = year - 1900;
      t.tm_mon  = month - 1;
      t.tm_mday = day;
      t.tm_hour = hour;
      t.tm_min  = minute;
      t.tm_sec  = 0;
      time_t epoch = mktime(&t) - TZ_OFFSET_SEC; // mktime은 로컬로 처리하므로 보정
      // 사실상: UTC 시간을 struct tm에 넣고 time_t로 변환하려면
      // 간단히 +9시간
      hour += 9;
      if (hour >= 24) {
        hour -= 24;
        day += 1; // 다음날로 넘어가면 오늘 일정에서 제외될 것
      }
    }
  }
}

// 오늘 날짜 구하기
int todayYear, todayMonth, todayDay;

void getToday() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    todayYear  = 2026; todayMonth = 7; todayDay = 13; // 폴백
    return;
  }
  todayYear  = timeinfo.tm_year + 1900;
  todayMonth = timeinfo.tm_mon + 1;
  todayDay   = timeinfo.tm_mday;
}

// ICS 텍스트에서 오늘 일정 추출
void parseICS(const String& icsData, const char* calName) {
  int pos = 0;
  while (pos < (int)icsData.length() && eventCount < MAX_EVENTS) {
    // VEVENT 블록 찾기
    int veventStart = icsData.indexOf("BEGIN:VEVENT", pos);
    if (veventStart < 0) break;
    int veventEnd = icsData.indexOf("END:VEVENT", veventStart);
    if (veventEnd < 0) break;

    String vevent = icsData.substring(veventStart, veventEnd);

    // SUMMARY
    String summary = "";
    int sumIdx = vevent.indexOf("SUMMARY:");
    if (sumIdx >= 0) {
      int lineEnd = vevent.indexOf('\n', sumIdx);
      if (lineEnd < 0) lineEnd = vevent.length();
      summary = vevent.substring(sumIdx + 8, lineEnd);
      summary.trim();
    }

    // LOCATION
    String location = "";
    int locIdx = vevent.indexOf("LOCATION:");
    if (locIdx >= 0) {
      int lineEnd = vevent.indexOf('\n', locIdx);
      if (lineEnd < 0) lineEnd = vevent.length();
      location = vevent.substring(locIdx + 9, lineEnd);
      location.trim();
    }

    // DTSTART
    String dtStart = "";
    int dsIdx = vevent.indexOf("DTSTART");
    if (dsIdx >= 0) {
      int colonIdx = vevent.indexOf(':', dsIdx);
      int lineEnd = vevent.indexOf('\n', colonIdx);
      if (lineEnd < 0) lineEnd = vevent.length();
      dtStart = vevent.substring(colonIdx + 1, lineEnd);
      dtStart.trim();
    }

    // DTEND
    String dtEnd = "";
    int deIdx = vevent.indexOf("DTEND");
    if (deIdx >= 0) {
      int colonIdx = vevent.indexOf(':', deIdx);
      int lineEnd = vevent.indexOf('\n', colonIdx);
      if (lineEnd < 0) lineEnd = vevent.length();
      dtEnd = vevent.substring(colonIdx + 1, lineEnd);
      dtEnd.trim();
    }

    // 날짜 파싱
    int sYear, sMonth, sDay, sHour, sMin;
    bool sAllDay;
    parseDateTime(dtStart, sYear, sMonth, sDay, sHour, sMin, sAllDay);

    int eYear, eMonth, eDay, eHour, eMin;
    bool eAllDay;
    parseDateTime(dtEnd, eYear, eMonth, eDay, eHour, eMin, eAllDay);

    // 오늘 일정인지 확인
    bool isToday = false;
    if (sAllDay) {
      // 종일: 시작일~종료일 범위에 오늘이 포함되는지
      if (sYear == todayYear && sMonth == todayMonth && sDay == todayDay) {
        isToday = true;
      }
      // 여러 날에 걸친 종일 일정
      if (!isToday && sDay <= todayDay && eDay >= todayDay &&
          sMonth == todayMonth && eMonth == todayMonth) {
        isToday = true;
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
      events[eventCount].calName   = calName;
      eventCount++;
    }

    pos = veventEnd + 10;
  }
}

// ====== 일정 정렬 (시간순) ======
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

// ====== ICS 다운로드 ======
String fetchICS(const char* url) {
  HTTPClient http;
  String payload = "";

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(5000);  // 5초 타임아웃 (절전: 실패 시 빠른 복귀)
  http.setConnectTimeout(3000);  // 연결 타임아웃 3초

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

// ====== E-Paper 표시 ======
void displayCalendar() {
  display.setRotation(1); // 가로 모드: 416 x 240
  display.setFont(&FreeSansBold9pt7b);
  display.setTextColor(GxEPD_BLACK);

  const int W = display.width();   // 416
  const int H = display.height(); // 240

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

    display.setFont(&FreeSansBold18pt7b);
    int16_t tbx, tby; uint16_t tbw, tbh;
    display.getTextBounds(dateStr, 0, 0, &tbx, &tby, &tbw, &tbh);
    display.setCursor(10, 24);
    display.print(dateStr);

    // 구분선
    display.drawLine(0, 30, W, 30, GxEPD_BLACK);

    // ---- 일정 목록 ----
    int y = 46;
    const int lineHeight = 22;
    const int maxVisible = 8; // 화면에 표시 가능한 최대 일정 수

    if (eventCount == 0) {
      display.setFont(&FreeSans12pt7b);
      display.setCursor(20, y + 20);
      display.print("오늘 일정이 없습니다");
    } else {
      int showCount = min(eventCount, maxVisible);
      for (int i = 0; i < showCount; i++) {
        Event& ev = events[i];

        // 시간 표시
        display.setFont(&FreeSans9pt7b);
        char timeBuf[20];
        if (ev.allDay) {
          snprintf(timeBuf, sizeof(timeBuf), "종일");
        } else {
          snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d-%02d:%02d",
                   ev.startHour, ev.startMin, ev.endHour, ev.endMin);
        }
        display.setCursor(6, y);
        display.print(timeBuf);

        // 캘린더 색상 마커 (작은 사각형)
        // 흑백 e-paper이므로 캘린더별 다른 패턴 사용
        int markerX = 82;
        if (ev.calName == "calendar1") {
          display.fillRect(markerX, y - 8, 6, 8, GxEPD_BLACK);
        } else if (ev.calName == "calendar2") {
          display.drawRect(markerX, y - 8, 6, 8, GxEPD_BLACK);
        } else if (ev.calName == "calendar3") {
          display.fillRect(markerX, y - 8, 6, 4, GxEPD_BLACK);
        } else if (ev.calName == "calendar4") {
          display.fillRect(markerX, y - 4, 6, 4, GxEPD_BLACK);
        }

        // 일정 제목
        display.setFont(&FreeSansBold9pt7b);
        display.setCursor(92, y);
        // 제목이 길면 자르기
        String title = ev.summary;
        if (title.length() > 24) {
          title = title.substring(0, 23) + "..";
        }
        display.print(title);

        // 위치 (있으면 다음 줄에 작게)
        if (ev.location.length() > 0 && y + 14 < H - 10) {
          display.setFont(&FreeSans9pt7b);
          display.setCursor(92, y + 14);
          String loc = ev.location;
          if (loc.length() > 24) {
            loc = loc.substring(0, 23) + "..";
          }
          display.print(loc);
          y += lineHeight + 14;
        } else {
          y += lineHeight;
        }

        // 구분선 (일정 사이)
        if (i < showCount - 1 && y < H - 10) {
          display.drawFastHLine(6, y - 6, W - 12, GxEPD_BLACK);
        }
      }

      // 더 많은 일정이 있으면 안내
      if (eventCount > maxVisible) {
        display.setFont(&FreeSans9pt7b);
        display.setCursor(W - 80, H - 6);
        char moreBuf[20];
        snprintf(moreBuf, sizeof(moreBuf), "+%d개 더", eventCount - maxVisible);
        display.print(moreBuf);
      }
    }

    // ---- 하단: 업데이트 시간 ----
    display.setFont(&FreeSans9pt7b);
    char updateStr[30];
    snprintf(updateStr, sizeof(updateStr), "업데이트 %02d:%02d",
             timeinfo.tm_hour, timeinfo.tm_min);
    display.setCursor(6, H - 4);
    display.print(updateStr);

    // 캘린더 범례
    display.setFont(&FreeSans9pt7b);
    int legendX = W - 160;
    int legendY = H - 4;
    // calendar1
    display.fillRect(legendX, legendY - 8, 6, 8, GxEPD_BLACK);
    display.setCursor(legendX + 8, legendY);
    display.print("Cal1");
    // calendar2
    legendX += 38;
    display.drawRect(legendX, legendY - 8, 6, 8, GxEPD_BLACK);
    display.setCursor(legendX + 8, legendY);
    display.print("Cal2");
    // calendar3
    legendX += 38;
    display.fillRect(legendX, legendY - 8, 6, 4, GxEPD_BLACK);
    display.setCursor(legendX + 8, legendY);
    display.print("Cal3");
    // calendar4
    legendX += 38;
    display.fillRect(legendX, legendY - 4, 6, 4, GxEPD_BLACK);
    display.setCursor(legendX + 8, legendY);
    display.print("Cal4");

  } while (display.nextPage());
}

// ====== 에러 표시 (e-paper) ======
void displayError(const char* msg) {
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(20, 60);
    display.print("Error");
    display.setFont(&FreeSans9pt7b);
    display.setCursor(20, 85);
    display.print(msg);
  } while (display.nextPage());
}

// ====== WiFi 연결 ======
bool connectWiFi() {
  Serial.printf("WiFi 연결 중: %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // 절전: 연결 타임아웃 10초 (기존 20초)
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
    WiFi.disconnect(true);  // WiFi 완전 해제 (전력 누수 방지)
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

// ====== WiFi 절전 해제 ======
void powerOffWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi OFF (절전)");
}

// ====== E-Paper 전원 차단 (슬립 중 누설 전류 감소) ======
void powerOffEPaper() {
  // RST 핀을 LOW로 설정하여 e-paper 모듈 전원 차단
  pinMode(EPD_RST_PIN, OUTPUT);
  digitalWrite(EPD_RST_PIN, LOW);
  Serial.println("E-Paper 전원 차단 (절전)");
}

// ====== 딥슬립 진입 ======
void enterDeepSleep() {
  Serial.printf("딥슬립 진입 (%d분 후 갱신)\n", REFRESH_INTERVAL_MIN);

  // WiFi 끄기
  powerOffWiFi();

  // E-Paper 전원 차단
  powerOffEPaper();

  // 불필요한 GPIO 플로팅 방지 (누설 전류 감소)
  // SPI 핀 풀다운
  pinMode(18, INPUT_PULLDOWN);  // MOSI
  pinMode(19, INPUT_PULLDOWN);  // SCK
  pinMode(EPD_CS_PIN, INPUT_PULLDOWN);
  pinMode(EPD_DC_PIN, INPUT_PULLDOWN);
  pinMode(EPD_BUSY_PIN, INPUT_PULLDOWN);

  // 딥슬립 타이머 설정
  esp_sleep_enable_timer_wakeup((uint64_t)REFRESH_INTERVAL_MIN * 60 * 1000000ULL);
  esp_deep_sleep_start();
}

// ====== 메인 일정 갱신 ======
void refreshCalendar() {
  eventCount = 0;
  getToday();

  Serial.printf("오늘: %d-%02d-%02d\n", todayYear, todayMonth, todayDay);

  int failCount = 0;
  for (int i = 0; i < NUM_CALENDARS; i++) {
    Serial.printf("캘린더 '%s' 다운로드 중...\n", calendars[i].name);
    String icsData = fetchICS(calendars[i].url);
    if (icsData.length() > 0) {
      Serial.printf("  → %d bytes 수신\n", icsData.length());
      parseICS(icsData, calendars[i].name);
    } else {
      Serial.printf("  → 수신 실패\n");
      failCount++;
    }
  }

  // 절전: 모든 캘린더 실패 시 즉시 슬립 (불필요한 WiFi 유지 방지)
  if (failCount == NUM_CALENDARS) {
    Serial.println("모든 캘린더 다운로드 실패 → 즉시 슬립");
    displayError("캘린더 로드 실패");
    display.hibernate();
    enterDeepSleep();
    return;  // 여기 도달 안함
  }

  Serial.printf("총 %d개 일정\n", eventCount);
  sortEvents();
  displayCalendar();
}

// ====== Arduino setup/loop ======
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) { delay(10); }
  Serial.println("\n=== Google Calendar E-Power Display ===");

  // 디스플레이 초기화
  display.init(115200, true, 50, false);
  display.hibernate();

  // WiFi 연결
  if (!connectWiFi()) {
    displayError("WiFi 연결 실패");
    return;
  }

  // NTP 시간 동기화
  if (!syncTime()) {
    displayError("시간 동기화 실패");
    return;
  }

  // 일정 가져와서 표시
  refreshCalendar();

  // 디스플레이 절전
  display.hibernate();

  // 딥슬립 진입 (WiFi OFF + E-Paper 전원차단 + GPIO 플로팅 방지)
  enterDeepSleep();
}

void loop() {
  // 딥슬립을 사용하므로 loop는 실행되지 않음
}
