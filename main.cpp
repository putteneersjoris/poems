#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

Adafruit_SSD1306 oled(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
U8G2_FOR_ADAFRUIT_GFX u8g2;

const char* ssid = "pbcourt";
const char* password = "42pbcourt";

const char* POEMS_URL = "https://raw.githubusercontent.com/putteneersjoris/poems/refs/heads/main/poems.json";
const char* IPLOC_URL = "http://ip-api.com/json/?fields=lat,lon,city";
const char* OWM_KEY = "cb66a744575c1047a0991d78c6a32f01";

const long TZ_OFFSET_SEC = 7 * 3600;  // Bangkok UTC+7; change to 3600 for Belgium CET
const int  DST_OFFSET   = 0;

const unsigned long SCROLL_INTERVAL_MS = 200;
const int LINE_H = 11;
const int MAX_LINE_CHARS_BUF = 256;

const unsigned long WEATHER_REFRESH_MS = 10UL * 60UL * 1000UL;
const unsigned long CLOCK_REDRAW_MS = 1000;

void debugScreen(const String& msg);

struct PoemLine { String text; };
struct Poem { String title; String author; std::vector<String> lines; };

Poem currentPoem;
bool poemLoaded = false;

float weatherTemp = NAN;
String weatherDesc = "";
unsigned long lastWeatherFetch = 0;
double locLat = 0, locLon = 0;
bool locResolved = false;

enum Mode { MODE_CLOCK, MODE_POEM };
Mode mode = MODE_CLOCK;
int lastPoemHourTriggered = -1;

unsigned long lastScrollTime = 0;
unsigned long lastClockDraw = 0;
int scrollOffset = 0;
int totalHeight = 0;

bool fetchUrl(const String& url, String& out) {
  HTTPClient http;
  bool ok = false;
  if (url.startsWith("https")) {
    WiFiClientSecure client;
    client.setInsecure();
    if (http.begin(client, url)) {
      int code = http.GET();
      if (code == 200) { out = http.getString(); ok = true; }
      http.end();
    }
  } else {
    if (http.begin(url)) {
      int code = http.GET();
      if (code == 200) { out = http.getString(); ok = true; }
      http.end();
    }
  }
  return ok;
}

bool resolveLocation() {
  String body;
  if (!fetchUrl(IPLOC_URL, body)) return false;
  DynamicJsonDocument doc(512);
  if (deserializeJson(doc, body)) return false;
  locLat = doc["lat"] | 0.0;
  locLon = doc["lon"] | 0.0;
  return locLat != 0;
}

bool fetchWeather() {
  if (!locResolved) return false;
  String url = "https://api.openweathermap.org/data/2.5/weather?lat=" + String(locLat, 4)
             + "&lon=" + String(locLon, 4) + "&units=metric&appid=" + OWM_KEY;
  String body;
  if (!fetchUrl(url, body)) return false;
  DynamicJsonDocument doc(2048);
  if (deserializeJson(doc, body)) return false;
  weatherTemp = doc["main"]["temp"] | NAN;
  weatherDesc = (const char*)(doc["weather"][0]["main"] | "");
  return true;
}

bool fetchRandomPoem() {
  // debugScreen("Fetching...");
  String body;
  if (!fetchUrl(POEMS_URL, body)) {
    // debugScreen("fetchUrl FAIL");
    return false;
  }
  // debugScreen("got " + String(body.length()) + "B heap=" + String(ESP.getFreeHeap()));

  DynamicJsonDocument doc(65536);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    // debugScreen(String("JSON err: ") + err.c_str());
    return false;
  }
  JsonArray arr = doc["poems"].as<JsonArray>();
  int n = arr.size();
  // debugScreen("poems n=" + String(n));
  if (n == 0) return false;
  int idx = random(n);
  // debugScreen("idx=" + String(idx));
  JsonObject p = arr[idx];
  currentPoem.title = (const char*)(p["title"] | "");
  currentPoem.author = (const char*)(p["author"] | "");
  currentPoem.lines.clear();
  for (JsonVariant v : p["lines"].as<JsonArray>()) {
    currentPoem.lines.push_back(String((const char*)v));
  }
  poemLoaded = true;
  return true;
}

std::vector<String> wrapLines(const std::vector<String>& src, int maxW) {
  std::vector<String> out;
  u8g2.setFont(u8g2_font_6x10_tf);
  for (const String& line : src) {
    if (line.length() == 0) { out.push_back(""); continue; }
    String cur = "";
    int i = 0;
    while (i < (int)line.length()) {
      int sp = line.indexOf(' ', i);
      if (sp < 0) sp = line.length();
      String word = line.substring(i, sp);
      String tryLine = cur.length() ? cur + " " + word : word;
      if (u8g2.getUTF8Width(tryLine.c_str()) <= maxW) {
        cur = tryLine;
      } else {
        if (cur.length()) out.push_back(cur);
        cur = word;
      }
      i = sp + 1;
    }
    if (cur.length()) out.push_back(cur);
  }
  return out;
}

std::vector<String> wrappedLines;

void preparePoem() {
  std::vector<String> head;
  head.push_back(currentPoem.title);
  if (currentPoem.author.length()) head.push_back("  - " + currentPoem.author);
  head.push_back("");
  for (auto& l : currentPoem.lines) head.push_back(l);
  wrappedLines = wrapLines(head, SCREEN_WIDTH);
  scrollOffset = -SCREEN_HEIGHT;
  totalHeight = wrappedLines.size() * LINE_H + 32;
}

void debugScreen(const String& msg) {
  oled.clearDisplay();
  u8g2.setFont(u8g2_font_6x10_tf);
  int y = 10;
  int start = 0;
  while (start < (int)msg.length() && y < SCREEN_HEIGHT) {
    int end = start + 21;
    if (end > (int)msg.length()) end = msg.length();
    u8g2.setCursor(0, y);
    u8g2.print(msg.substring(start, end));
    start = end;
    y += 11;
  }
  oled.display();
  delay(2000);
}

void drawPoem() {
  oled.clearDisplay();
  u8g2.setFont(u8g2_font_6x10_tf);
  for (size_t i = 0; i < wrappedLines.size(); i++) {
    int y = (int)i * LINE_H - scrollOffset;
    if (y > -LINE_H && y < SCREEN_HEIGHT + LINE_H) {
      u8g2.setCursor(0, y + LINE_H - 2);
      u8g2.print(wrappedLines[i]);
    }
  }
  oled.display();
}

void drawClock() {
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  char timeStr[12];
  snprintf(timeStr, sizeof(timeStr), "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
  char dateStr[16];
  snprintf(dateStr, sizeof(dateStr), "%02d/%02d/%04d", t.tm_mday, t.tm_mon + 1, t.tm_year + 1900);

  oled.clearDisplay();

  oled.setTextColor(WHITE);
  oled.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  oled.getTextBounds(timeStr, 0, 0, &x1, &y1, &w, &h);
  int tx = (SCREEN_WIDTH - w) / 2;
  oled.setCursor(tx, 4);
  oled.print(timeStr);

  oled.setTextSize(1);
  oled.setCursor(tx, 26);
  oled.print(dateStr);

  u8g2.setFont(u8g2_font_6x10_tf);
  String w1 = weatherDesc.length() ? weatherDesc : String("--");
  String w2 = isnan(weatherTemp) ? String("--") : String(weatherTemp, 1) + "C";
  u8g2.setCursor(tx, 48);
  u8g2.print(w1);
  u8g2.setCursor(tx, 60);
  u8g2.print(w2);

  oled.display();
}

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  if (!oled.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 failed");
    while (true);
  }
  u8g2.begin(oled);
  u8g2.setFontMode(1);
  u8g2.setFontDirection(0);
  u8g2.setForegroundColor(WHITE);

  oled.clearDisplay();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(0, 10);
  u8g2.print("Connecting WiFi...");
  oled.display();

  WiFi.begin(ssid, password);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);

  configTime(TZ_OFFSET_SEC, DST_OFFSET, "pool.ntp.org", "time.nist.gov");

  randomSeed(esp_random());

  locResolved = resolveLocation();
  fetchWeather();
  lastWeatherFetch = millis();
}

void loop() {
  unsigned long now = millis();

  time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);

  if (mode == MODE_CLOCK) {
    if (lt.tm_hour >= 11 && lt.tm_hour <= 23 && lt.tm_min == 0 && lt.tm_sec == 0 && lt.tm_hour != lastPoemHourTriggered) {

      if (fetchRandomPoem()) {
        preparePoem();
        mode = MODE_POEM;
        lastPoemHourTriggered = lt.tm_hour;
      }
    }
    if (now - lastWeatherFetch >= WEATHER_REFRESH_MS) {
      fetchWeather();
      lastWeatherFetch = now;
    }
    if (now - lastClockDraw >= CLOCK_REDRAW_MS) {
      drawClock();
      lastClockDraw = now;
    }
  } else {
    if (now - lastScrollTime >= SCROLL_INTERVAL_MS) {
      scrollOffset++;
      if (scrollOffset >= totalHeight) {
        mode = MODE_CLOCK;
        lastClockDraw = 0;
      } else {
        drawPoem();
      }
      lastScrollTime = now;
    }
  }
}