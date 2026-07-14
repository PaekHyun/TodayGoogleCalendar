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
 *   - U8g2_for_Adafruit_GFX (by oliver)  ← 한글 폰트 지원
 * 
 * 보드 설정: XIAO_ESP32C6 (esp32 by Espressif v2.x+)
 */

#define ENABLE_GxEPD2_GFX 0

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

// ====== E-Paper 핀 설정 (XIAO ESP32C6) ======
#define EPD_CS_PIN   0   // D0 / GPIO0
#define EPD_DC_PIN   1   // D1 / GPIO1
#define EPD_RST_PIN  2   // D2 / GPIO2
#define EPD_BUSY_PIN 3   // D3 / GPIO3
// SPI: D8=SCK(19), D10=MOSI(18)

GxEPD2_BW<GxEPD2_370_GDEY037T03, GxEPD2_370_GDEY037T03::HEIGHT> display(
  GxEPD2_370_GDEY037T03(EPD_CS_PIN, EPD_DC_PIN, EPD_RST_PIN, EPD_BUSY_PIN)
);

U8G2_FOR_ADAFRUIT_GFX u8g2Fonts; // 한글 폰트 렌더러

// ====== 캘린더 설정 ======
// ★ 이름과 URL을 여기서만 수정하면 마커/범례/표시 모두 자동 반영됩니다
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
  int    calIndex;  // calendars[] 배열의 인덱스 → 이름 참조용
};

const int MAX_EVENTS = 30;
Event events[MAX_EVENTS];
int    eventCount = 0;

// ====== 캘린더 마커 그리기 (인덱스로 자동 결정) ======
void drawCalMarker(int x, int y, int calIndex) {
  switch (calIndex % 4) {
    case 0: display.fillRect(x, y - 8, 6, 8, GxEPD_BLACK); break;  // ■ 꽉 찬 사각형
    case 1: display.drawRect(x, y - 8, 6, 8, GxEPD_BLACK); break;  // □ 빈 사각형
    case 2: display.fillRect(x, y - 8, 6, 4, GxEPD_BLACK); break;  // ▀ 위쪽 반
    case 3: display.fillRect(x, y - 4, 6, 4, GxEPD_BLACK); break;  // ▄ 아래쪽 반
  }
}

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
      hour += 9;
      if (hour >= 24) {
        hour -= 24;
        day += 1;
      }
    }
  }
}

// 오늘 날짜 구하기
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

// ICS 텍스트에서 오늘 일정 추출
void parseICS(const String& icsData, int calIndex) {
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
      if (sYear == todayYear && sMonth == todayMonth && sDay == todayDay) {
        isToday = true;
      }
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
      events[eventCount].calIndex  = calIndex;
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

    u8g2Fonts.setFont(u8g2_font_unifont_t_korean1);
    u8g2Fonts.setCursor(8, 16);
    u8g2Fonts.print(dateStr);

    // 구분선
    display.drawLine(0, 22, W, 22, GxEPD_BLACK);

    // ---- 일정 목록 ----
    int y = 38;
    const int lineHeight = 20;
    const int maxVisible = 8;

    if (eventCount == 0) {
      u8g2Fonts.setFont(u8g2_font_unifont_t_korean1);
      u8g2Fonts.setCursor(20, y + 10);
      u8g2Fonts.print("오늘 일정이 없습니다");
    } else {
      int showCount = min(eventCount, maxVisible);
      for (int i = 0; i < showCount; i++) {
        Event& ev = events[i];

        // 시간 표시
        char timeBuf[20];
        if (ev.allDay) {
          snprintf(timeBuf, sizeof(timeBuf), "종일");
        } else {
          snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d-%02d:%02d",
                   ev.startHour, ev.startMin, ev.endHour, ev.endMin);
        }
        u8g2Fonts.setFont(u8g2_font_unifont_t_korean1);
        u8g2Fonts.setCursor(4, y);
        u8g2Fonts.print(timeBuf);

        // 캘린더 마커 (calIndex로 자동 결정)
        drawCalMarker(82, y, ev.calIndex);

        // 일정 제목
        u8g2Fonts.setCursor(92, y);
        String title = ev.summary;
        if (title.length() > 18) {
          title = title.substring(0, 17) + "..";
        }
        u8g2Fonts.print(title);

        // 위치 (있으면 다음 줄에 작게)
        if (ev.location.length() > 0 && y + 14 < H - 20) {
          u8g2Fonts.setCursor(92, y + 14);
          String loc = ev.location;
          if (loc.length() > 18) {
            loc = loc.substring(0, 17) + "..";
          }
          u8g2Fonts.print(loc);
          y += lineHeight + 14;
        } else {
          y += lineHeight;
        }

        // 구분선 (일정 사이)
        if (i < showCount - 1 && y < H - 20) {
          display.drawFastHLine(4, y - 4, W - 8, GxEPD_BLACK);
        }
      }

      // 더 많은 일정이 있으면 안내
      if (eventCount > maxVisible) {
        u8g2Fonts.setFont(u8g2_font_unifont_t_korean1);
        u8g2Fonts.setCursor(W - 80, H - 6);
        char moreBuf[20];
        snprintf(moreBuf, sizeof(moreBuf), "+%d개 더", eventCount - maxVisible);
        u8g2Fonts.print(moreBuf);
      }
    }

    // ---- 하단: 업데이트 시간 ----
    u8g2Fonts.setFont(u8g2_font_unifont_t_korean1);
    char updateStr[30];
    snprintf(updateStr, sizeof(updateStr), "%02d:%02d 갱신",
             timeinfo.tm_hour, timeinfo.tm_min);
    u8g2Fonts.setCursor(4, H - 4);
    u8g2Fonts.print(updateStr);

    // ---- 하단: 캘린더 범례 (calendars[]에서 자동 생성) ----
    int legendX = W - 40 * NUM_CALENDARS;
    int legendY = H - 4;
    for (int i = 0; i < NUM_CALENDARS; i++) {
      drawCalMarker(legendX, legendY, i);
      u8g2Fonts.setCursor(legendX + 8, legendY);
      u8g2Fonts.print(calendars[i].name);
      legendX += 40;
    }

  } while (display.nextPage());
}

// ====== 에러 표시 (e-paper) ======
void displayError(const char* msg) {
  display.setRotation(1);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFont(u8g2_font_unifont_t_korean1);
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
  pinMode(EPD_RST_PIN, OUTPUT);
  digitalWrite(EPD_RST_PIN, LOW);
  Serial.println("E-Paper 전원 차단 (절전)");
}

// ====== 딥슬립 진입 ======
void enterDeepSleep() {
  Serial.printf("딥슬립 진입 (%d분 후 갱신)\n", REFRESH_INTERVAL_MIN);

  powerOffWiFi();
  powerOffEPaper();

  // 불필요한 GPIO 플로팅 방지 (누설 전류 감소)
  pinMode(18, INPUT_PULLDOWN);  // MOSI
  pinMode(19, INPUT_PULLDOWN);  // SCK
  pinMode(EPD_CS_PIN, INPUT_PULLDOWN);
  pinMode(EPD_DC_PIN, INPUT_PULLDOWN);
  pinMode(EPD_BUSY_PIN, INPUT_PULLDOWN);

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
      parseICS(icsData, i);
    } else {
      Serial.printf("  → 수신 실패\n");
      failCount++;
    }
  }

  // 절전: 모든 캘린더 실패 시에도 표시 후 슬립
  if (failCount == NUM_CALENDARS) {
    Serial.println("모든 캘린더 다운로드 실패");
    // 빈 화면이라도 표시 (에러 메시지 포함)
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

  // 디스플레이 초기화 (diagnostic 출력 끄기: baud=0)
  display.init(0, true, 50, false);

  // 한글 폰트 초기화
  u8g2Fonts.begin(display);

  // WiFi 연결 (실패 시 30초 대기 후 재부팅)
  if (!connectWiFi()) {
    displayError("WiFi 연결 실패\n30초 후 재시도");
    display.hibernate();
    delay(30000);
    ESP.restart();
  }

  // NTP 시간 동기화 (실패 시 30초 대기 후 재부팅)
  if (!syncTime()) {
    displayError("시간 동기화 실패\n30초 후 재시도");
    display.hibernate();
    delay(30000);
    ESP.restart();
  }

  // 일정 가져와서 표시
  refreshCalendar();

  // 디스플레이 절전
  display.hibernate();

  // 딥슬립 진입
  enterDeepSleep();
}

void loop() {
  // 딥슬립을 사용하므로 loop는 실행되지 않음
}
