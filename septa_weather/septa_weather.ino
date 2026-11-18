/*
  SEPTA Route 40 arrivals + weather for the Elecrow CrowPanel ESP32-S3 5.79" e-paper (792x272)

  Arduino IDE settings (same as the Elecrow demo that worked):
    Board: ESP32S3 Dev Module
    PSRAM: OPI PSRAM
    Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
    Upload Speed: 115200

  This folder must also contain Elecrow's display files from their weather demo
  (not included in this repo; see README):
    EPD.cpp  EPD.h  EPD_Init.cpp  EPD_Init.h  EPDfont.h  spi.cpp  spi.h
  and your own secrets.h (copy secrets.example.h and fill it in).
  Library needed (Library Manager): Arduino_JSON by Arduino
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include <time.h>
#include <math.h>
#include "EPD.h"
#include "sans_fonts.h"  // DejaVu Sans bitmap fonts

// Give the main loop more memory than the 8 KB default (secure web requests + JSON need room)
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

// ===================== YOUR SETTINGS =====================
// Wi-Fi name, password and OpenWeather key live in secrets.h (same folder),
// so you can replace this file in future updates without retyping them.
#include "secrets.h"
const char* YOUR_NAME     = "Friend";   // shown as "Good Morning, Friend!"

// Weather location: SEPTA's listed coordinates for the Lombard St & 10th St stop
const char* LAT = "39.943821";
const char* LON = "-75.158258";

// SEPTA
const char* ROUTE_ID     = "40";
const int   DIRECTION_ID = 1;      // the direction that serves stop 22265
const long  STOP_ID      = 22265;  // Lombard St & 10th St
const char* STOP_LABEL   = "10th & Lombard St";
const bool  SHOW_SCHEDULED = true; // false = show only GPS-tracked buses
const int   MAX_ROWS = 4;

// Timing
const unsigned long BUS_INTERVAL_MS     = 60UL * 1000;       // fallback when the clock isn't set
// Bus data: every minute when the next bus is within BUS_NEAR_MINUTES, otherwise every BUS_FAR_INTERVAL_MS.
// (Each download is ~900 KB, so fewer downloads = less data. The countdown still updates every minute.)
const int           BUS_NEAR_MINUTES    = 20;
const unsigned long BUS_FAR_INTERVAL_MS = 5UL * 60 * 1000;
const unsigned long WEATHER_INTERVAL_MS = 15UL * 60 * 1000;  // weather every 15 minutes
// The screen does a full (flashing) clean refresh every N updates and a quieter
// partial refresh otherwise. If partial updates look wrong, set this to 1.
const int FULL_REFRESH_EVERY = 30;

// Overnight pause: no bus downloads from PAUSE_START_HOUR to PAUSE_END_HOUR (24-hour clock).
// Clock and weather keep updating. Set both to 0 to never pause.
const int PAUSE_START_HOUR = 0;   // midnight
const int PAUSE_END_HOUR   = 6;   // 6 AM

const char* TZ_INFO = "EST5EDT,M3.2.0,M11.1.0";  // Philadelphia time zone
// =========================================================

#define SCR_W 792
#define SCR_H 272
#define DIV_X 372   // vertical divider between weather (left) and buses (right)
#define RX    386   // left edge of the bus panel
#define DEG   "\x7f"  // degree sign in the sans fonts
#define NSLOTS 4    // forecast columns (3 hours apart)

uint8_t ImageBW[27200];  // screen buffer (Elecrow's library uses 800x272 internally)

// ---------- data ----------
// kind: 0 = schedule only, 1 = in SEPTA real-time feed (no vehicle listed), 2 = real-time prediction with a vehicle
struct Arrival { long eta; uint8_t kind; char headsign[40]; };
Arrival arrivals[MAX_ROWS];
int  arrivalCount = 0;
bool busOk = false;
bool busPaused = false;

struct Slot { time_t t; int temp; char main[16]; };
struct Cand { char tripId[16]; long eta; uint8_t kind; char headsign[40]; };  // bus candidate while merging feeds
Cand cand[20];
int  candN = 0;
Slot slots[NSLOTS];
int  slotCount = 0;

bool haveCurrent = false;
int  curTemp = 0, feelsLike = 0, humidity = 0, rainPct = -1;
char curMain[16] = "";

int screenUpdates = 0;

// ===================== small helpers =====================
long jLong(JSONVar v) {
  String t = JSON.typeof(v);
  if (t == "number") return (long)(double)v;
  if (t == "string") return atol((const char*)v);
  return 0;
}
double jDouble(JSONVar v) {
  String t = JSON.typeof(v);
  if (t == "number") return (double)v;
  if (t == "string") return atof((const char*)v);
  return 0;
}
bool jBool(JSONVar v) {
  String t = JSON.typeof(v);
  if (t == "boolean") return (bool)v;
  if (t == "number") return (double)v != 0;
  if (t == "string") return String((const char*)v) == "true";
  return false;
}
String jStr(JSONVar v) {
  String t = JSON.typeof(v);
  if (t == "string") return String((const char*)v);
  if (t == "number") return String((long)(double)v);
  return "";
}

bool timeValid() { return time(nullptr) > 1700000000; }

bool inPause(int hour) {
  if (PAUSE_START_HOUR == PAUSE_END_HOUR) return false;
  if (PAUSE_START_HOUR < PAUSE_END_HOUR) return hour >= PAUSE_START_HOUR && hour < PAUSE_END_HOUR;
  return hour >= PAUSE_START_HOUR || hour < PAUSE_END_HOUR;  // window crosses midnight, e.g. 23 to 6
}

// GET a URL; returns the body on HTTP 200, otherwise "". 'label' keeps keys out of the log.
String httpGet(const String& url, const char* label) {
  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;
  if (url.startsWith("https")) {
    secureClient.setInsecure();  // skips certificate checking (simpler; not verified)
    http.begin(secureClient, url);
  } else {
    http.begin(plainClient, url);
  }
  http.setTimeout(15000);
  int code = http.GET();
  String body;
  if (code == 200) body = http.getString();
  Serial.printf("GET %s -> %d (%u bytes)\n", label, code, body.length());
  http.end();
  return body;
}

// ===================== weather =====================
void fetchWeather() {
  if (strlen(OWM_API_KEY) == 0) { Serial.println("No OpenWeather key set"); return; }
  String base = String("http://api.openweathermap.org/data/2.5/");
  String q = String("?lat=") + LAT + "&lon=" + LON + "&units=metric&appid=" + OWM_API_KEY;

  String body = httpGet(base + "weather" + q, "weather");
  if (body.length()) {
    JSONVar root = JSON.parse(body);
    body = "";
    if (JSON.typeof(root) == "object") {
      curTemp   = lround(jDouble(root["main"]["temp"]));
      feelsLike = lround(jDouble(root["main"]["feels_like"]));
      humidity  = lround(jDouble(root["main"]["humidity"]));
      strncpy(curMain, jStr(root["weather"][0]["main"]).c_str(), sizeof(curMain) - 1);
      haveCurrent = true;
      Serial.printf("Now: %dC feels %dC, %d%%, %s\n", curTemp, feelsLike, humidity, curMain);
    }
  }

  body = httpGet(base + "forecast" + q + "&cnt=" + String(NSLOTS), "forecast");
  if (body.length()) {
    JSONVar root = JSON.parse(body);
    body = "";
    JSONVar list = root["list"];
    if (JSON.typeof(list) == "array") {
      int n = list.length();
      if (n > NSLOTS) n = NSLOTS;
      double maxPop = 0;
      for (int i = 0; i < n; i++) {
        slots[i].t    = (time_t)jLong(list[i]["dt"]);
        slots[i].temp = lround(jDouble(list[i]["main"]["temp"]));
        memset(slots[i].main, 0, sizeof(slots[i].main));
        strncpy(slots[i].main, jStr(list[i]["weather"][0]["main"]).c_str(), sizeof(slots[i].main) - 1);
        double p = jDouble(list[i]["pop"]);
        if (p > maxPop) maxPop = p;
      }
      slotCount = n;
      rainPct = lround(maxPop * 100);
      Serial.printf("Forecast slots: %d, max rain chance %d%%\n", slotCount, rainPct);
    }
  }
}

// ===================== SEPTA =====================
// Parses SEPTA's schedule time, e.g. "10/20/26 10:19 am", as Philadelphia local time.
long parseSeptaTime(const char* s) {
  int mo, d, y, h, mi; char ap[3] = {0};
  if (sscanf(s, "%d/%d/%d %d:%d %2s", &mo, &d, &y, &h, &mi, ap) != 6) return 0;
  if (h == 12) h = 0;
  if (ap[0] == 'p' || ap[0] == 'P') h += 12;
  struct tm t = {};
  t.tm_year = (y < 100 ? y + 2000 : y) - 1900;
  t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_isdst = -1;
  return (long)mktime(&t);
}

int findCand(const String& id) {
  for (int i = 0; i < candN; i++) if (id == cand[i].tripId) return i;
  return -1;
}

// ---------- SEPTA official real-time feed (GTFS-Realtime, protobuf) ----------
// This is the system-wide feed apps like Google use. It's ~900 KB, so it's loaded into PSRAM.
uint8_t* feedBuf = nullptr;
size_t   feedCap = 0;

bool pbVarint(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
  v = 0; int sh = 0;
  while (p < end && sh < 64) { uint8_t b = *p++; v |= (uint64_t)(b & 0x7F) << sh; if (!(b & 0x80)) return true; sh += 7; }
  return false;
}
// Reads the next field. For length-delimited fields, sets [s, e). Returns false at end or on bad data.
bool pbField(const uint8_t*& p, const uint8_t* end, uint32_t& fn, uint32_t& wt, uint64_t& v,
             const uint8_t*& s, const uint8_t*& e) {
  if (p >= end) return false;
  uint64_t key;
  if (!pbVarint(p, end, key)) return false;
  fn = key >> 3; wt = key & 7;
  if (wt == 0) return pbVarint(p, end, v);
  if (wt == 2) { uint64_t len; if (!pbVarint(p, end, len) || len > (uint64_t)(end - p)) return false; s = p; e = p + len; p = e; return true; }
  if (wt == 1) { if (end - p < 8) return false; p += 8; return true; }
  if (wt == 5) { if (end - p < 4) return false; p += 4; return true; }
  return false;
}
bool pbStrEq(const uint8_t* s, const uint8_t* e, const char* str) {
  size_t n = strlen(str);
  return (size_t)(e - s) == n && memcmp(s, str, n) == 0;
}

// Downloads the feed into PSRAM. Returns its length, or 0 on failure.
size_t downloadFeed() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);  // plain (non-chunked) body
  http.begin(client, "https://www3.septa.org/gtfsrt/septa-pa-us/Trip/rtTripUpdates.pb");
  http.setTimeout(20000);
  int code = http.GET();
  int size = http.getSize();
  if (code != 200 || size <= 0) {
    Serial.printf("GET SEPTA realtime feed -> %d (size %d)\n", code, size);
    http.end();
    return 0;
  }
  if ((size_t)size > feedCap) {
    if (feedBuf) free(feedBuf);
    feedCap = size + 256 * 1024;  // room to grow
    feedBuf = (uint8_t*)ps_malloc(feedCap);
    if (!feedBuf) { feedCap = 0; Serial.println("Not enough PSRAM for feed (is PSRAM set to OPI PSRAM?)"); http.end(); return 0; }
  }
  WiFiClient* stream = http.getStreamPtr();
  size_t got = 0;
  unsigned long start = millis();
  while (got < (size_t)size && millis() - start < 60000) {
    size_t avail = stream->available();
    if (avail) {
      size_t want = (size_t)size - got;
      if (want > 4096) want = 4096;
      got += stream->readBytes(feedBuf + got, want);
    } else if (!stream->connected()) {
      break;
    } else {
      delay(2);
    }
  }
  http.end();
  Serial.printf("GET SEPTA realtime feed -> %d (%u of %d bytes, %lu ms)\n", code, got, size, millis() - start);
  return got == (size_t)size ? got : 0;
}

// Pulls our route/direction/stop out of the feed into cand[]. Returns false if the feed couldn't be read.
bool parseFeed(size_t len, long now) {
  const uint8_t* p = feedBuf; const uint8_t* end = feedBuf + len;
  uint32_t fn, wt; uint64_t v; const uint8_t *s = nullptr, *e = nullptr;
  char stopId[12]; snprintf(stopId, sizeof(stopId), "%ld", STOP_ID);
  int found = 0;
  while (pbField(p, end, fn, wt, v, s, e)) {
    if (fn != 2 || wt != 2) continue;                        // FeedMessage.entity
    const uint8_t* ep = s; const uint8_t* ee = e;
    while (pbField(ep, ee, fn, wt, v, s, e)) {
      if (fn != 3 || wt != 2) continue;                      // FeedEntity.trip_update
      const uint8_t* tp = s; const uint8_t* te = e;
      char tripId[16] = ""; bool routeOk = false; long dir = -1; bool hasVehicle = false; long when = 0;
      while (pbField(tp, te, fn, wt, v, s, e)) {
        if (fn == 1 && wt == 2) {                            // TripUpdate.trip (TripDescriptor)
          const uint8_t* dp = s; const uint8_t* de = e;
          while (pbField(dp, de, fn, wt, v, s, e)) {
            if (fn == 1 && wt == 2) { size_t n = e - s; if (n > 15) n = 15; memcpy(tripId, s, n); tripId[n] = 0; }
            else if (fn == 5 && wt == 2) routeOk = pbStrEq(s, e, ROUTE_ID);
            else if (fn == 6 && wt == 0) dir = (long)v;
          }
        } else if (fn == 3 && wt == 2) {                     // TripUpdate.vehicle
          hasVehicle = (e > s);
        } else if (fn == 2 && wt == 2 && when == 0) {        // TripUpdate.stop_time_update
          const uint8_t* up = s; const uint8_t* ue = e;
          bool isOurs = false; long arr = 0, dep = 0;
          while (pbField(up, ue, fn, wt, v, s, e)) {
            if (fn == 4 && wt == 2) isOurs = pbStrEq(s, e, stopId);
            else if ((fn == 2 || fn == 3) && wt == 2) {      // arrival / departure StopTimeEvent
              const uint8_t* xp = s; const uint8_t* xe = e; uint32_t f2, w2; uint64_t v2; const uint8_t *s2, *e2;
              while (pbField(xp, xe, f2, w2, v2, s2, e2)) if (f2 == 2 && w2 == 0) { if (fn == 2) arr = (long)v2; else dep = (long)v2; }
            }
          }
          if (isOurs) when = arr ? arr : dep;
        }
      }
      if (!routeOk || (dir >= 0 && dir != DIRECTION_ID) || when == 0) continue;
      if (when < now - 30) continue;                          // already passed
      int k = findCand(String(tripId));
      if (k < 0 && candN < 20) { k = candN++; memset(&cand[k], 0, sizeof(Cand)); strncpy(cand[k].tripId, tripId, sizeof(cand[k].tripId) - 1); }
      if (k >= 0) { cand[k].eta = when < now ? now : when; cand[k].kind = hasVehicle ? 2 : 1; found++; }
    }
  }
  Serial.printf("Realtime feed: %d upcoming trip(s) for our stop\n", found);
  return true;
}

// Combines SEPTA's official real-time feed (predictions, like Google shows) with the
// schedule for our stop (later trips the real-time feed doesn't cover yet).
bool fetchBuses() {
  if (!timeValid()) { Serial.println("Clock not set yet"); return false; }
  long now = (long)time(nullptr);
  candN = 0;
  bool anyOk = false;

  // ---- 1. schedule at our stop (also gives each trip's destination) ----
  String body = httpGet(String("https://www3.septa.org/api/BusSchedules/index.php?stop_id=") + String(STOP_ID), "SEPTA schedule");
  if (body.length()) {
    JSONVar root = JSON.parse(body);
    body = "";
    if (JSON.typeof(root) == "object") {
      JSONVar list = root[ROUTE_ID];
      if (JSON.typeof(list) == "array") {
        anyOk = true;
        int m = list.length();
        for (int i = 0; i < m && candN < 20; i++) {
          if (jLong(list[i]["Direction"]) != DIRECTION_ID) continue;
          long t = parseSeptaTime(jStr(list[i]["DateCalender"]).c_str());
          if (t == 0) continue;
          Cand& c = cand[candN++];
          memset(&c, 0, sizeof(Cand));
          strncpy(c.tripId, jStr(list[i]["trip_id"]).c_str(), sizeof(c.tripId) - 1);
          strncpy(c.headsign, jStr(list[i]["DirectionDesc"]).c_str(), sizeof(c.headsign) - 1);
          c.eta = t;
        }
      }
    }
  }

  // ---- 2. real-time predictions ----
  size_t len = downloadFeed();
  if (len && parseFeed(len, now)) anyOk = true;
  if (!anyOk) return false;

  // ---- 3. destination names for buses already running (the real-time feed doesn't include them) ----
  bool needNames = false;
  for (int i = 0; i < candN; i++) if (cand[i].headsign[0] == 0) needNames = true;
  if (needNames) {
    body = httpGet(String("https://www3.septa.org/api/v2/trips/?route_id=") + ROUTE_ID, "SEPTA trips");
    if (body.length()) {
      JSONVar trips = JSON.parse(body);
      body = "";
      if (JSON.typeof(trips) == "array") {
        int m = trips.length();
        for (int i = 0; i < m; i++) {
          int k = findCand(jStr(trips[i]["trip_id"]));
          if (k >= 0 && cand[k].headsign[0] == 0)
            strncpy(cand[k].headsign, jStr(trips[i]["trip_headsign"]).c_str(), sizeof(cand[k].headsign) - 1);
        }
      }
    }
  }

  // ---- 4. keep upcoming ones, soonest first ----
  Arrival found[20];
  int count = 0;
  for (int i = 0; i < candN; i++) {
    Cand& c = cand[i];
    if (c.kind == 0 && !SHOW_SCHEDULED) continue;
    if (c.kind == 0 && c.eta < now - 60) continue;   // scheduled time passed, not in real-time feed
    found[count].eta = c.eta;
    found[count].kind = c.kind;
    memcpy(found[count].headsign, c.headsign, sizeof(found[count].headsign));
    if (found[count].headsign[0] == 0) strcpy(found[count].headsign, "Westbound");
    Serial.printf("  trip %s %s: %ld min %s\n", c.tripId, found[count].headsign, (c.eta - now) / 60,
                  c.kind == 2 ? "(real-time, vehicle)" : c.kind == 1 ? "(real-time)" : "(schedule only)");
    count++;
  }
  for (int a = 1; a < count; a++) {
    Arrival key = found[a];
    int b = a - 1;
    while (b >= 0 && found[b].eta > key.eta) { found[b + 1] = found[b]; b--; }
    found[b + 1] = key;
  }
  arrivalCount = count < MAX_ROWS ? count : MAX_ROWS;
  for (int i = 0; i < arrivalCount; i++) arrivals[i] = found[i];
  return true;
}

// ===================== drawing primitives =====================
void px(int x, int y, uint16_t c = BLACK) {
  if (x < 0 || y < 0 || x >= SCR_W || y >= SCR_H) return;
  Paint_SetPixel(x, y, c);
}
void fillRect(int x0, int y0, int x1, int y1, uint16_t c) {
  for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px(x, y, c);
}
void fillCircle(int cx, int cy, int r, uint16_t c) {
  if (r <= 0) return;
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++)
      if (dx * dx + dy * dy <= r * r) px(cx + dx, cy + dy, c);
}
void ring(int cx, int cy, int r, int th) {
  int ri = r - th;
  for (int dy = -r; dy <= r; dy++)
    for (int dx = -r; dx <= r; dx++) {
      int d = dx * dx + dy * dy;
      if (d <= r * r && d > ri * ri) px(cx + dx, cy + dy);
    }
}
void thickLine(int x0, int y0, int x1, int y1, int th) {
  for (int o = 0; o < th; o++) {
    for (int p = 0; p < th; p++) {
      int ax = x0 + o, ay = y0 + p, bx = x1 + o, by = y1 + p;
      // simple Bresenham with bounds-checked pixels
      int dx = abs(bx - ax), sx = ax < bx ? 1 : -1;
      int dy = -abs(by - ay), sy = ay < by ? 1 : -1;
      int err = dx + dy;
      while (true) {
        px(ax, ay);
        if (ax == bx && ay == by) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; ax += sx; }
        if (e2 <= dx) { err += dx; ay += sy; }
      }
    }
  }
}

// Text in DejaVu Sans. (x, y) is the top-left of the line. Cuts the text to fit before maxX.
int charWidth(char c, const SansFont& f) {
  unsigned char u = (unsigned char)c;
  if (u < 32 || u > 127) u = '?';
  return f.glyphs[u - 32].adv;
}
int textWidth(const char* s, const SansFont& f) {
  int w = 0;
  for (; *s; s++) w += charWidth(*s, f);
  return w;
}
int text(int x, int y, const char* s, const SansFont& f, int maxX = SCR_W) {
  int base = y + f.ascent;
  int start = x;
  for (; *s; s++) {
    unsigned char u = (unsigned char)*s;
    if (u < 32 || u > 127) u = '?';
    const SansGlyph& g = f.glyphs[u - 32];
    if (x + g.adv > maxX) break;
    const uint8_t* bits = f.bits + g.offset;
    for (int r = 0; r < g.h; r++)
      for (int c = 0; c < g.w; c++) {
        int i = r * g.w + c;
        if (bits[i >> 3] & (0x80 >> (i & 7))) px(x + g.xo + c, base + g.yo + r);
      }
    x += g.adv;
  }
  return x - start;
}
void textRight(int right, int y, const char* s, const SansFont& f) { text(right - textWidth(s, f), y, s, f); }
void textCenter(int cx, int y, const char* s, const SansFont& f) { text(cx - textWidth(s, f) / 2, y, s, f); }
// Draws with the first (largest) font in the list that fits before maxX.
void textFit(int x, int y, const char* s, const SansFont* const* fonts, int n, int maxX) {
  for (int i = 0; i < n; i++) {
    if (x + textWidth(s, *fonts[i]) <= maxX || i == n - 1) { text(x, y, s, *fonts[i], maxX); return; }
  }
}

// ===================== icons =====================
void iconSun(int cx, int cy, int s) {
  ring(cx, cy, s * 42 / 100, s >= 30 ? 3 : 2);
  for (int a = 0; a < 360; a += 45) {
    float rad = a * PI / 180.0;
    int x0 = cx + cos(rad) * s * 60 / 100, y0 = cy + sin(rad) * s * 60 / 100;
    int x1 = cx + cos(rad) * s * 88 / 100, y1 = cy + sin(rad) * s * 88 / 100;
    thickLine(x0, y0, x1, y1, s >= 30 ? 2 : 1);
  }
}
void iconMoon(int cx, int cy, int s) {
  fillCircle(cx, cy, s * 55 / 100, BLACK);
  fillCircle(cx + s * 30 / 100, cy - s * 20 / 100, s * 48 / 100, WHITE);
}
void cloudShape(int cx, int cy, int s, uint16_t c, int in) {
  fillCircle(cx - s * 50 / 100, cy + s * 10 / 100, s * 38 / 100 - in, c);
  fillCircle(cx + s * 5 / 100,  cy - s * 15 / 100, s * 52 / 100 - in, c);
  fillCircle(cx + s * 55 / 100, cy + s * 15 / 100, s * 33 / 100 - in, c);
  fillRect(cx - s * 50 / 100, cy + s * 10 / 100, cx + s * 55 / 100, cy + s * 48 / 100 - in, c);
}
void iconCloud(int cx, int cy, int s) {
  cloudShape(cx, cy, s, BLACK, 0);
  cloudShape(cx, cy, s, WHITE, s >= 30 ? 3 : 2);
}
void iconRain(int cx, int cy, int s) {
  iconCloud(cx, cy - s * 25 / 100, s * 85 / 100);
  for (int i = -1; i <= 1; i++) {
    int x = cx + i * s * 35 / 100;
    thickLine(x, cy + s * 35 / 100, x - s * 12 / 100, cy + s * 75 / 100, s >= 30 ? 2 : 1);
  }
}
void iconSnow(int cx, int cy, int s) {
  iconCloud(cx, cy - s * 25 / 100, s * 85 / 100);
  for (int i = -1; i <= 1; i++) fillCircle(cx + i * s * 35 / 100, cy + s * 55 / 100, s >= 30 ? 3 : 2, BLACK);
}
void iconThunder(int cx, int cy, int s) {
  iconCloud(cx, cy - s * 25 / 100, s * 85 / 100);
  int th = s >= 30 ? 2 : 1;
  thickLine(cx + s * 10 / 100, cy + s * 30 / 100, cx - s * 10 / 100, cy + s * 55 / 100, th);
  thickLine(cx - s * 10 / 100, cy + s * 55 / 100, cx + s * 10 / 100, cy + s * 55 / 100, th);
  thickLine(cx + s * 10 / 100, cy + s * 55 / 100, cx - s * 10 / 100, cy + s * 85 / 100, th);
}
void iconFog(int cx, int cy, int s) {
  int th = s >= 30 ? 3 : 2;
  int lens[4] = {80, 60, 80, 50};
  for (int i = 0; i < 4; i++) {
    int y = cy - s * 45 / 100 + i * s * 30 / 100;
    int half = s * lens[i] / 100;
    fillRect(cx - half, y, cx + half, y + th - 1, BLACK);
  }
}
bool isNight(int hour) { return hour < 6 || hour >= 19; }
void weatherIcon(const char* mainStr, int cx, int cy, int s, bool night) {
  String m(mainStr);
  if (m.length() == 0) return;
  if (m == "Clear")                    { if (night) iconMoon(cx, cy, s); else iconSun(cx, cy, s); }
  else if (m == "Clouds")              iconCloud(cx, cy, s);
  else if (m == "Rain" || m == "Drizzle") iconRain(cx, cy, s);
  else if (m == "Thunderstorm")        iconThunder(cx, cy, s);
  else if (m == "Snow")                iconSnow(cx, cy, s);
  else                                 iconFog(cx, cy, s);  // mist, haze, fog, etc.
}

// "GPS" signal arcs next to tracked buses. (x, y) is the bottom-left corner.
void liveIcon(int x, int y, int arcs = 2) {
  fillCircle(x + 2, y - 2, 3, BLACK);
  for (int i = 0; i < arcs; i++) {
    int r = 9 + i * 7;
    for (int a = 0; a <= 90; a += 2) {
      float rad = a * PI / 180.0;
      for (int t = 0; t < 3; t++) px(x + 2 + cos(rad) * (r + t), y - 2 - sin(rad) * (r + t));
    }
  }
}

// ===================== screen layout =====================
void hourLabel(time_t t, char* out, size_t n) {
  struct tm lt; localtime_r(&t, &lt);
  int h = lt.tm_hour % 12; if (h == 0) h = 12;
  snprintf(out, n, "%d%s", h, lt.tm_hour < 12 ? "am" : "pm");
}

void drawScreen() {
  Paint_Clear(WHITE);
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  char buf[64];
  const int LM = 10;            // left margin
  const int LMAX = DIV_X - 8;   // right limit of the weather panel

  // ---------- left: weather ----------
  const char* part = "Hello";
  if (timeValid()) {
    if (lt.tm_hour >= 5 && lt.tm_hour < 12) part = "Good Morning";
    else if (lt.tm_hour >= 12 && lt.tm_hour < 17) part = "Good Afternoon";
    else if (lt.tm_hour >= 17 && lt.tm_hour < 22) part = "Good Evening";
    else part = "Good Night";
  }
  snprintf(buf, sizeof(buf), "%s, %s!", part, YOUR_NAME);
  static const SansFont* const greetFonts[] = { &SANSB26, &SANSB22 };
  textFit(LM, 6, buf, greetFonts, 2, LMAX);

  if (haveCurrent) {
    weatherIcon(curMain, 46, 84, 36, timeValid() && isNight(lt.tm_hour));
    snprintf(buf, sizeof(buf), "%d" DEG "C", curTemp);
    text(100, 30, buf, SANSB76, LMAX);
  } else {
    text(LM, 70, "Weather unavailable", SANS22, LMAX);
  }

  // "Feels like 13°C   Precipitation: 20%" on one line, as large as fits
  buf[0] = 0;
  if (haveCurrent) snprintf(buf, sizeof(buf), "Feels like %d" DEG "C", feelsLike);
  if (rainPct >= 0) {
    size_t l = strlen(buf);
    snprintf(buf + l, sizeof(buf) - l, "%sPrecipitation: %d%%", l ? "   " : "", rainPct);
  }
  static const SansFont* const lineFonts[] = { &SANS22, &SANS20, &SANS17 };
  textFit(LM, 128, buf, lineFonts, 3, LMAX);

  int colW = (LMAX - LM) / NSLOTS;
  for (int i = 0; i < slotCount; i++) {
    int cx = LM + colW * i + colW / 2;
    hourLabel(slots[i].t, buf, sizeof(buf));
    textCenter(cx, 162, buf, SANS20);
    snprintf(buf, sizeof(buf), "%d" DEG "C", slots[i].temp);
    textCenter(cx, 186, buf, SANS20);
    struct tm st; localtime_r(&slots[i].t, &st);
    weatherIcon(slots[i].main, cx, 238, 24, isNight(st.tm_hour));
  }

  // divider
  fillRect(DIV_X, 6, DIV_X + 1, SCR_H - 7, BLACK);

  // ---------- right: buses ----------
  text(RX, 2, "Arrivals", SANSB30);
  if (timeValid()) {
    int h = lt.tm_hour % 12; if (h == 0) h = 12;
    snprintf(buf, sizeof(buf), "%d:%02d %s", h, lt.tm_min, lt.tm_hour < 12 ? "AM" : "PM");
    textRight(SCR_W - 6, 2, buf, SANSB30);
  }
  text(RX, 36, STOP_LABEL, SANS17);

  if (busPaused) {
    int eh = PAUSE_END_HOUR % 12; if (eh == 0) eh = 12;
    text(RX, 76, "Bus times paused overnight", SANS22);
    snprintf(buf, sizeof(buf), "Back at %d:00 %s", eh, PAUSE_END_HOUR < 12 ? "AM" : "PM");
    text(RX, 104, buf, SANS17);
  } else if (!busOk) {
    text(RX, 76, "Can't reach SEPTA", SANS22);
  } else if (arrivalCount == 0) {
    text(RX, 76, SHOW_SCHEDULED ? "No buses on the way" : "No tracked buses", SANS22);
  }
  const int right = SCR_W - 32;  // right edge of the minutes, leaving room for the GPS icon
  for (int i = 0; i < arrivalCount; i++) {
    int y0 = 62 + i * 52;
    text(RX, y0, ROUTE_ID, SANSB30);
    long mins = (arrivals[i].eta - (long)now) / 60;
    int mw;
    if (mins <= 0) {
      mw = textWidth("Due", SANSB30);
      text(right - mw, y0, "Due", SANSB30);
    } else {
      snprintf(buf, sizeof(buf), "%ld", mins);
      int nw = textWidth(buf, SANSB30), uw = textWidth(" min", SANS17);
      mw = nw + uw;
      text(right - mw, y0, buf, SANSB30);
      text(right - uw, y0 + 12, " min", SANS17);   // small "min", same baseline
    }
    if (arrivals[i].kind == 2) liveIcon(right + 8, y0 + 24, 2);       // real-time prediction, vehicle assigned
    else if (arrivals[i].kind == 1) liveIcon(right + 8, y0 + 24, 1);  // real-time prediction, no vehicle listed
    text(RX + 48, y0 + 12, arrivals[i].headsign, SANS17, right - mw - 8);
  }
}

// Push the buffer to the panel. 'full' = clean refresh (flashes), as in Elecrow's demo.
void pushScreen(bool full) {
  if (full) {
    EPD_FastMode1Init();
    EPD_Display_Clear();
    EPD_Update();
    EPD_Clear_R26A6H();
  } else {
    EPD_HW_RESET();  // wake from deep sleep, as in Elecrow's partial-refresh example
  }
  EPD_Display(ImageBW);
  EPD_PartUpdate();
  EPD_DeepSleep();
}

void showMessage(const char* msg) {
  Paint_Clear(WHITE);
  text(20, 120, msg, SANS22);
  pushScreen(true);
}

// ===================== Wi-Fi & clock =====================
bool connectWifi(unsigned long timeoutMs) {
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.print("Connecting to Wi-Fi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Connected, IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("Wi-Fi not connected");
  return false;
}

// ===================== main =====================
unsigned long lastBus = 0, lastWeather = 0;
bool firstRun = true;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Booting");

  // Connect Wi-Fi first, before powering the screen (same order as Elecrow's weather demo)
  WiFi.setAutoReconnect(true);
  while (!connectWifi(30000)) {
    delay(5000);
  }

  configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");
  unsigned long start = millis();
  while (!timeValid() && millis() - start < 20000) delay(250);
  Serial.println(timeValid() ? "Clock set" : "Clock not set yet (will keep trying)");

  pinMode(7, OUTPUT);       // screen power (from Elecrow's demo)
  digitalWrite(7, HIGH);
  EPD_GPIOInit();
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
}

int lastMinute = -1;

void loop() {
  connectWifi(15000);

  if (firstRun || millis() - lastWeather >= WEATHER_INTERVAL_MS) {
    fetchWeather();
    lastWeather = millis();
  }

  // Redraw at the start of every new minute so the clock doesn't skip minutes.
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  bool due;
  if (timeValid()) due = (lt.tm_min != lastMinute);
  else due = firstRun || millis() - lastBus >= BUS_INTERVAL_MS;

  if (due) {
    lastMinute = lt.tm_min;
    busPaused = timeValid() && inPause(lt.tm_hour);
    if (busPaused) {
      arrivalCount = 0;
      Serial.println("Overnight pause: skipping bus data");
    } else {
      // Is the next bus close? (Uses the times from the last download.)
      long nextMin = 9999;
      if (busOk && arrivalCount > 0) nextMin = (arrivals[0].eta - (long)now) / 60;
      bool near = nextMin <= BUS_NEAR_MINUTES;
      if (firstRun || !busOk || lastBus == 0 || near || millis() - lastBus >= BUS_FAR_INTERVAL_MS - 5000) {
        busOk = fetchBuses();
        lastBus = millis();
      } else {
        Serial.printf("Next bus in %ld min: reusing bus data from %lu s ago\n", nextMin, (millis() - lastBus) / 1000);
      }
    }
    Serial.println("Drawing screen");
    drawScreen();
    bool full = (screenUpdates % FULL_REFRESH_EVERY == 0);
    Serial.printf("Sending to screen (%s refresh)\n", full ? "full" : "partial");
    pushScreen(full);
    screenUpdates++;
    Serial.println("Screen updated");
  }
  firstRun = false;
  delay(500);
}
