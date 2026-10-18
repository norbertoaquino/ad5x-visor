#include <Arduino.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"

#include "config.h"

#ifndef WIFI_CHANNEL
#define WIFI_CHANNEL 0
#endif

#include "flashforge_logo.h"
#include "i18n.h"
#include "ui_font.h"

static volatile bool gDebug = DEBUG_LOG;
static const int kBootPin = 0;

static const int BL_PIN = 40;
static const int CX = 120;
static const int CY = 120;
static const uint32_t kScreenMs = 5000;
static const uint32_t kPollMs = 30000;
static const uint32_t kHttpConnectMs = 20000;
static const uint32_t kHttpReadMs = 30000;
static const int kArcStart = 45;
static const int kArcSpan = 270;

TFT_eSPI tft;

struct Job {
  bool ok = false;
  int percent = 0;
  int layer = 0;
  int layers = 0;
  int nozzle = -1;
  int nozzleTarget = -1;
  int bed = -1;
  int bedTarget = -1;
  int elapsedSec = -1;
  int remainSec = -1;
  String status;
  String file;
};

static uint8_t* gBmp = nullptr;
static int gBmpLen = 0;
static int gBmpCap = 0;
static uint16_t* gImg = nullptr;
static int gImgW = 0;
static int gImgH = 0;
static String gImgFile;
static uint32_t gThumbAttemptAt = 0;
static bool gThumbHeld = false;
static volatile bool gThumbWipe = false;
static int gScreen = 0;
static int gShownKind = 0;
static volatile int gPrinterMiss = 0;
static volatile bool gPrinterOff = false;
static const uint32_t kPingMs = 5000;
static SemaphoreHandle_t gPingDone = nullptr;
static uint32_t gShownAt = 0;
static Job gLast;
static bool gHave = false;
static bool gLink = false;
static uint32_t gHaloAt = 0;
static bool gSawWork = false;
static bool gHoldDone = false;
static SemaphoreHandle_t gDataMu = nullptr;

void lockData() {
  if (gDataMu) xSemaphoreTake(gDataMu, portMAX_DELAY);
}

void unlockData() {
  if (gDataMu) xSemaphoreGive(gDataMu);
}

void logStamp() {
  time_t now = time(nullptr);
  struct tm parts;
  if (now > 1700000000 && localtime_r(&now, &parts)) {
    Serial.printf("%02d:%02d:%02d ", parts.tm_hour, parts.tm_min, parts.tm_sec);
    return;
  }
  uint32_t sec = millis() / 1000;
  Serial.printf("%02lu:%02lu:%02lu ", (unsigned long)(sec / 3600), (unsigned long)((sec / 60) % 60),
                (unsigned long)(sec % 60));
}

void logln(Msg id) {
  if (!gDebug) return;
  logStamp();
  Serial.println(tr(id));
}

void logf(Msg id, ...) {
  if (!gDebug) return;
  char buf[640];
  va_list args;
  va_start(args, id);
  vsnprintf(buf, sizeof(buf), tr(id), args);
  va_end(args);
  size_t n = strlen(buf);
  if (n && buf[n - 1] == '\n') buf[n - 1] = 0;
  logStamp();
  Serial.println(buf);
}

void logPrinterBody(Msg title, const uint8_t* data, int len) {
  if (!gDebug) return;
  int show = len;
  int binAt = -1;
  if (data && len > 8000) {
    int scan = 800;
    for (int i = 0; i + 1 < len && i < scan; i++) {
      if (data[i] == 'B' && data[i + 1] == 'M') {
        binAt = i;
        break;
      }
    }
    if (binAt >= 0) show = binAt;
  }
  if (show < 0) show = 0;
  logf(MSG_HEAD, tr(title), len);
  if (!data || show <= 0) logln(MSG_EMPTY);
  else {
    const int chunk = 128;
    for (int i = 0; i < show; i += chunk) {
      int n = show - i;
      if (n > chunk) n = chunk;
      Serial.write(data + i, n);
      vTaskDelay(1);
    }
    Serial.println();
  }
  if (binAt >= 0) logf(MSG_BIN, len - binAt);
  logln(MSG_END);
}

void serviceHalo();
void netTask(void*);

static volatile bool gHaveIp = false;
static volatile bool gAssociated = false;
static volatile int gWifiPhase = 0;

uint16_t c565(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(r, g, b);
}

const uint16_t kBg = 0x0000;
const uint16_t kTrack = 0x18E3;
const uint16_t kInk = 0xFFFF;
const uint16_t kMuted = 0x6B6D;
const uint16_t kAccent = 0x05BF;
const uint16_t kHot = 0xF984;
const uint16_t kBed = 0x3CDF;

void backlight(uint8_t duty) {
  ledcSetup(0, 5000, 8);
  ledcAttachPin(BL_PIN, 0);
  ledcWrite(0, duty);
}

void linkLed(bool ok) {
  tft.fillCircle(CX, 18, 2, ok ? c565(40, 190, 80) : c565(210, 48, 48));
}

bool thumbReady() {
  lockData();
  bool ok = gThumbHeld && gImg && gImgW > 0 && gImgH > 0;
  unlockData();
  return ok;
}

bool showTimes(const Job& job) {
  return job.status == "printing";
}

int screenCount(const Job& job) {
  return 2 + (showTimes(job) ? 1 : 0) + (thumbReady() ? 1 : 0);
}

void pages(int page, int n) {
  if (n < 1) n = 1;
  if (page < 0) page = 0;
  if (page >= n) page = n - 1;
  for (int i = 0; i < n; i++) {
    int x = CX - ((n - 1) * 14) / 2 + i * 14;
    tft.fillCircle(x, 208, i == page ? 2 : 1, i == page ? kAccent : kTrack);
  }
}

void formatHms(char* out, size_t n, int sec) {
  if (sec < 0) {
    snprintf(out, n, "--");
    return;
  }
  int h = sec / 3600;
  int m = (sec % 3600) / 60;
  int s = sec % 60;
  const char* hour = tr(MSG_UI_HOUR);
  const char* minute = tr(MSG_UI_MIN);
  const char* second = tr(MSG_UI_SEC);
  if (h > 0) snprintf(out, n, "%d%s %02d%s %02d%s", h, hour, m, minute, s, second);
  else if (m > 0) snprintf(out, n, "%d%s %02d%s", m, minute, s, second);
  else snprintf(out, n, "%d%s", s, second);
}

String jsonString(const String& src, const char* key) {
  String pat = String("\"") + key + "\":\"";
  int at = src.indexOf(pat);
  if (at < 0) return "";
  at += pat.length();
  int end = src.indexOf('"', at);
  if (end < 0) return "";
  return src.substring(at, end);
}

float jsonFloat(const String& src, const char* key) {
  String pat = String("\"") + key + "\":";
  int at = src.indexOf(pat);
  if (at < 0) return NAN;
  at += pat.length();
  if (src.charAt(at) == '"') return NAN;
  return src.substring(at).toFloat();
}

const char* prettyStatus(const String& raw) {
  if (raw == "printing") return tr(MSG_UI_PRINTING);
  if (raw == "pause" || raw == "paused") return tr(MSG_UI_PAUSE);
  if (raw == "heating") return tr(MSG_UI_HEATING);
  if (raw == "completed" || raw == "complete") return tr(MSG_UI_DONE);
  if (raw == "ready") return tr(MSG_UI_READY);
  if (raw == "busy" || raw == "calibrate_doing") return tr(MSG_UI_BUSY);
  return nullptr;
}

String baseName(String file) {
  int slash = file.lastIndexOf('/');
  if (slash >= 0) file = file.substring(slash + 1);
  file.replace(".gcode", "");
  file.replace(".GCODE", "");
  file.replace(".gco", "");
  file.replace(".3mf", "");
  file.replace(".3MF", "");
  return file;
}

const char* httpErrorName(int code) {
  switch (code) {
    case HTTPC_ERROR_CONNECTION_REFUSED: return tr(MSG_E_REFUSED);
    case HTTPC_ERROR_SEND_HEADER_FAILED: return tr(MSG_E_HEADER);
    case HTTPC_ERROR_SEND_PAYLOAD_FAILED: return tr(MSG_E_PAYLOAD);
    case HTTPC_ERROR_NOT_CONNECTED: return tr(MSG_E_NOT_CONN);
    case HTTPC_ERROR_CONNECTION_LOST: return tr(MSG_E_LOST);
    case HTTPC_ERROR_NO_STREAM: return tr(MSG_E_STREAM);
    case HTTPC_ERROR_NO_HTTP_SERVER: return tr(MSG_E_NOT_HTTP);
    case HTTPC_ERROR_TOO_LESS_RAM: return tr(MSG_E_RAM);
    case HTTPC_ERROR_ENCODING: return tr(MSG_E_ENCODING);
    case HTTPC_ERROR_STREAM_WRITE: return tr(MSG_E_WRITE);
    case HTTPC_ERROR_READ_TIMEOUT: return tr(MSG_READ_TIMEOUT);
    default: return tr(MSG_E_OTHER);
  }
}

bool queryPrinter(Job& job) {
  job = Job{};
  job.nozzle = job.nozzleTarget = job.bed = job.bedTarget = -1;
  HTTPClient http;
  String url = String("http://") + PRINTER_IP + ":8898/detail";
  if (!http.begin(url)) {
    logln(MSG_HTTP_BEGIN);
    return false;
  }
  http.setConnectTimeout(kHttpConnectMs);
  http.setTimeout(kHttpReadMs);
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"serialNumber\":\"") + PRINTER_SERIAL +
                "\",\"checkCode\":\"" + PRINTER_CHECK + "\"}";
  uint32_t started = millis();
  int code = http.POST(body);
  uint32_t spent = millis() - started;
  String payload = http.getString();
  http.end();
  logPrinterBody(MSG_DETAIL, (const uint8_t*)payload.c_str(), payload.length());
  if (code != 200 || payload.indexOf("\"code\":0") < 0) {
    const char* why = code < 0 ? httpErrorName(code) : (code != 200 ? tr(MSG_HTTP_STATUS) : tr(MSG_HTTP_CODE));
    logf(MSG_HTTP_LINE, code, why, (unsigned long)spent, (unsigned long)kHttpConnectMs,
         (unsigned long)kHttpReadMs, WiFi.RSSI(), WiFi.BSSIDstr().c_str(), (unsigned)payload.length());
    return false;
  }
  float raw = jsonFloat(payload, "printProgress");
  if (!isnan(raw)) job.percent = (int)((raw <= 1.0f ? raw * 100.0f : raw) + 0.5f);
  if (job.percent < 0) job.percent = 0;
  if (job.percent > 100) job.percent = 100;
  float layer = jsonFloat(payload, "printLayer");
  float layers = jsonFloat(payload, "targetPrintLayer");
  if (!isnan(layer)) job.layer = (int)layer;
  if (!isnan(layers)) job.layers = (int)layers;
  if (job.layer < 0 || job.layer > 9999) job.layer = 0;
  if (job.layers < 0 || job.layers > 9999) job.layers = 0;
  float nozzle = jsonFloat(payload, "rightTemp");
  float nozzleT = jsonFloat(payload, "rightTargetTemp");
  float bed = jsonFloat(payload, "platTemp");
  float bedT = jsonFloat(payload, "platTargetTemp");
  if (!isnan(nozzle)) job.nozzle = (int)(nozzle + 0.5f);
  if (!isnan(nozzleT)) job.nozzleTarget = (int)(nozzleT + 0.5f);
  if (!isnan(bed)) job.bed = (int)(bed + 0.5f);
  if (!isnan(bedT)) job.bedTarget = (int)(bedT + 0.5f);
  float elapsed = jsonFloat(payload, "printDuration");
  float remain = jsonFloat(payload, "estimatedTime");
  if (!isnan(elapsed) && elapsed >= 0 && elapsed < 1000000) job.elapsedSec = (int)(elapsed + 0.5f);
  if (!isnan(remain) && remain >= 0 && remain < 1000000) job.remainSec = (int)(remain + 0.5f);
  job.status = jsonString(payload, "status");
  job.file = jsonString(payload, "printFileName");
  job.ok = true;
  logf(MSG_PROG, job.percent, job.layer, job.layers, job.elapsedSec, job.remainSec, job.status.c_str(),
       job.file.c_str(), job.nozzle, job.nozzleTarget, job.bed, job.bedTarget);
  return true;
}

bool storeThumb(const String& file) {
  int32_t w = (int32_t)(gBmp[18] | (gBmp[19] << 8) | (gBmp[20] << 16) | (gBmp[21] << 24));
  int32_t h = (int32_t)(gBmp[22] | (gBmp[23] << 8) | (gBmp[24] << 16) | (gBmp[25] << 24));
  uint16_t bpp = gBmp[28] | (gBmp[29] << 8);
  uint32_t off = gBmp[10] | (gBmp[11] << 8) | (gBmp[12] << 16) | (gBmp[13] << 24);
  bool flip = h > 0;
  if (h < 0) h = -h;
  if (w < 1 || h < 1 || w > 220 || h > 180 || bpp != 24 || off >= (uint32_t)gBmpLen) {
    logf(MSG_THUMB_FIT, (long)w, (long)h, bpp, (unsigned long)off, gBmpLen);
    return false;
  }

  const int dw = w;
  const int dh = h;
  uint16_t* img = (uint16_t*)malloc((size_t)dw * dh * sizeof(uint16_t));
  if (!img) return false;

  int stride = ((w * 3 + 3) / 4) * 4;
  for (int dy = 0; dy < dh; dy++) {
    int src = flip ? (h - 1 - dy) : dy;
    uint32_t rowAt = off + (uint32_t)src * stride;
    if (rowAt + (uint32_t)w * 3 > (uint32_t)gBmpLen) {
      free(img);
      logf(MSG_THUMB_ROW, dy, (long)w, (long)h, gBmpLen);
      return false;
    }
    const uint8_t* rowp = gBmp + rowAt;
    uint16_t* dst = img + dy * dw;
    for (int dx = 0; dx < dw; dx++) {
      const uint8_t* p = rowp + dx * 3;
      dst[dx] = c565(p[2], p[1], p[0]);
    }
    if ((dy & 15) == 0) vTaskDelay(1);
  }

  lockData();
  free(gImg);
  gImg = img;
  gImgW = dw;
  gImgH = dh;
  gImgFile = file;
  unlockData();
  free(gBmp);
  gBmp = nullptr;
  gBmpLen = 0;
  gBmpCap = 0;
  return true;
}

int httpStatusCode(const uint8_t* buf, int len) {
  int i = 0;
  while (i < len && buf[i] != ' ') i++;
  i++;
  if (i >= len || buf[i] < '0' || buf[i] > '9') return -1;
  int code = 0;
  while (i < len && buf[i] >= '0' && buf[i] <= '9') {
    code = code * 10 + (buf[i] - '0');
    i++;
  }
  return code;
}

void logThumbFail(Msg why, uint32_t spent, int httpCode, int rawLen) {
  logf(MSG_THUMB_ERR, tr(why), (unsigned long)spent, httpCode, rawLen);
}

bool loadThumb(const String& file) {
  if (file.length() == 0) return false;
  if (gThumbHeld && gImg && baseName(gImgFile) == baseName(file)) return true;
  if (gImg && gImgFile == file) return true;
  lockData();
  free(gImg);
  gImg = nullptr;
  gImgW = 0;
  gImgH = 0;
  gImgFile = "";
  gThumbHeld = false;
  gThumbWipe = true;
  unlockData();
  free(gBmp);
  gBmp = nullptr;
  gBmpLen = 0;
  gBmpCap = 0;

  logln(MSG_THUMB_DOWN);

  WiFiClient client;
  uint32_t started = millis();
  if (!client.connect(PRINTER_IP, 8898, 15000)) {
    int err = errno;
    logf(MSG_THUMB_CONN, (unsigned long)(millis() - started), err, strerror(err));
    return false;
  }
  client.print("GET /getThum HTTP/1.0\r\nHost: ");
  client.print(PRINTER_IP);
  client.print("\r\nConnection: close\r\n\r\n");

  const int cap = 70000;
  gBmp = (uint8_t*)malloc(cap);
  if (!gBmp) {
    client.stop();
    logln(MSG_THUMB_MEM);
    return false;
  }
  gBmpCap = cap;
  memset(gBmp, 0, (size_t)cap);

  uint32_t start = millis();
  int got = 0;
  while (millis() - start < 40000 && got < cap - 1) {
    int avail = client.available();
    if (avail > 0) {
      int room = cap - 1 - got;
      if (room > 512) room = 512;
      if (room > avail) room = avail;
      int n = client.read(gBmp + got, room);
      if (n > 0) got += n;
      else vTaskDelay(1);
    } else if (!client.connected()) {
      break;
    } else {
      vTaskDelay(1);
    }
  }
  bool hung = client.connected() && got < cap - 1;
  client.stop();
  uint32_t spent = millis() - start;
  int rawLen = got;
  logPrinterBody(MSG_THUMB, gBmp, rawLen);
  uint8_t head[160];
  int headLen = got > (int)sizeof(head) ? (int)sizeof(head) : got;
  if (headLen > 0) memcpy(head, gBmp, headLen);
  int httpCode = httpStatusCode(head, headLen);

  int base = 0;
  for (int i = 0; i + 3 < got && i < 600; i++) {
    if (gBmp[i] == '\r' && gBmp[i + 1] == '\n' && gBmp[i + 2] == '\r' && gBmp[i + 3] == '\n') {
      base = i + 4;
      break;
    }
    if (gBmp[i] == '\n' && gBmp[i + 1] == '\n') {
      base = i + 2;
      break;
    }
  }
  if (base > 0 && base < got) {
    memmove(gBmp, gBmp + base, got - base);
    got -= base;
  }
  if (!(got > 2 && gBmp[0] == 'B' && gBmp[1] == 'M')) {
    int mark = -1;
    int limit = got - 2;
    if (limit > 800) limit = 800;
    for (int i = 0; i < limit; i++) {
      if (gBmp[i] == 'B' && gBmp[i + 1] == 'M') {
        mark = i;
        break;
      }
    }
    if (mark > 0) {
      memmove(gBmp, gBmp + mark, got - mark);
      got -= mark;
    }
  }
  if (got < 54 || gBmp[0] != 'B' || gBmp[1] != 'M') {
    gBmpLen = 0;
    Msg why = MSG_NOT_BMP;
    if (httpCode > 0 && httpCode != 200) why = MSG_HTTP_STATUS;
    else if (headLen <= 0) why = hung ? MSG_READ_TIMEOUT : MSG_EMPTY_REPLY;
    else if (hung) why = MSG_READ_TIMEOUT;
    logThumbFail(why, spent, httpCode, rawLen);
    return false;
  }
  gBmpLen = got;
  if (!storeThumb(file)) {
    logThumbFail(MSG_NO_FIT, spent, httpCode, rawLen);
    return false;
  }
  gThumbHeld = true;
  logf(MSG_THUMB_OK, got, file.c_str());
  return true;
}

void rule(int y, int value) {
  const int x = 54;
  const int w = 132;
  const int h = 3;
  tft.fillRect(x, y, w, h, kTrack);
  int fw = value < 0 ? 0 : value;
  if (fw > 100) fw = 100;
  fw = fw * w / 100;
  if (fw > 0) tft.fillRect(x, y, fw, h, kAccent);
}

void column(const char* label, int value, int target, int x, uint16_t accent) {
  uiDrawCenter(label, x, 68, accent, false);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(accent, kBg);
  char num[8];
  if (value < 0) snprintf(num, sizeof(num), "--");
  else snprintf(num, sizeof(num), "%d", value);
  tft.setTextColor(accent, kBg);
  tft.drawString(num, x, 116, 6);
  tft.setTextColor(kMuted, kBg);
  if (target < 0) tft.drawString("--", x, 162, 2);
  else {
    char goal[8];
    snprintf(goal, sizeof(goal), "%d", target);
    tft.drawString(goal, x, 162, 2);
  }
}

void drawOrb(int cx, int cy, const char* label, int value, int target, uint16_t accent) {
  const int outer = 34;
  const int hole = 28;
  tft.drawSmoothArc(cx, cy, outer, hole, 0, 360, kTrack, kBg, false);
  int sweep = 0;
  if (target > 0 && value > 0) {
    sweep = value * 360 / target;
    if (sweep > 360) sweep = 360;
  }
  if (sweep >= 350) {
    tft.drawSmoothArc(cx, cy, outer, hole, 0, 360, accent, kBg, false);
  } else if (sweep > 3) {
    int end = 180 + sweep;
    if (end <= 360) tft.drawSmoothArc(cx, cy, outer, hole, 180, end, accent, kBg, true);
    else {
      tft.drawSmoothArc(cx, cy, outer, hole, 180, 360, accent, kBg, false);
      tft.drawSmoothArc(cx, cy, outer, hole, 0, end - 360, accent, kBg, true);
    }
  }
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(accent, kBg);
  tft.drawString(label, cx, cy - 50, 2);
  char num[8];
  if (value < 0) snprintf(num, sizeof(num), "--");
  else snprintf(num, sizeof(num), "%d", value);
  tft.setTextColor(TFT_WHITE, kBg);
  tft.drawString(num, cx, cy - 8, 4);
  if (target >= 0) {
    char goal[8];
    snprintf(goal, sizeof(goal), "%d", target);
    tft.setTextColor(kMuted, kBg);
    tft.drawString(goal, cx, cy + 18, 2);
  }
}

void blitDisc(const uint16_t* img, int iw, int ih, int cx, int cy, int radius) {
  int diam = radius * 2;
  float cover = diam / (float)(iw < ih ? iw : ih);
  int dw = (int)(iw * cover + 0.5f);
  int dh = (int)(ih * cover + 0.5f);
  if (dw < 1) dw = 1;
  if (dh < 1) dh = 1;
  int ox = cx - dw / 2;
  int oy = cy - dh / 2;
  uint16_t row[160];
  for (int y = cy - radius; y <= cy + radius; y++) {
    int dy = y - cy;
    int dx = (int)sqrtf((float)radius * radius - (float)dy * dy);
    int n = dx * 2 + 1;
    if (n > 160) {
      dx = 79;
      n = 159;
    }
    int x0 = cx - dx;
    for (int i = 0; i < n; i++) {
      int sx = (x0 + i - ox) * iw / dw;
      int sy = (y - oy) * ih / dh;
      if (sx < 0) sx = 0;
      if (sy < 0) sy = 0;
      if (sx >= iw) sx = iw - 1;
      if (sy >= ih) sy = ih - 1;
      row[i] = img[sy * iw + sx];
    }
    tft.pushImage(x0, y, n, 1, row);
    if (((y + radius) & 7) == 0) vTaskDelay(1);
  }
}

void pill(const String& text, int y, uint16_t color) {
  if (!text.length()) return;
  int w = tft.textWidth(text, 2) + 22;
  if (w < 64) w = 64;
  if (w > 156) return;
  int x = CX - w / 2;
  tft.drawSmoothRoundRect(x, y - 12, 11, 9, w, 24, color, kBg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(color, kBg);
  tft.drawString(text, CX, y, 2);
}

int haloMode(const String& s) {
  bool error = s.indexOf("error") >= 0 || s.indexOf("fail") >= 0 || s.indexOf("fault") >= 0 ||
               s.indexOf("abort") >= 0 || s.indexOf("cancel") >= 0;
  bool done = s.indexOf("complet") >= 0 || s == "finished";
  bool work = s == "printing" || s == "heating" || s == "pause" || s == "paused" ||
              s == "busy" || s == "calibrate_doing";
  if (error) {
    gHoldDone = false;
    return 2;
  }
  if (work) {
    gSawWork = true;
    gHoldDone = false;
    return 0;
  }
  if (done || (gSawWork && s == "ready")) {
    gHoldDone = true;
    return 1;
  }
  if (gHoldDone) return 1;
  return 3;
}

void haloArc(int start, int span, uint16_t color) {
  if (span <= 0) return;
  while (start < 0) start += 360;
  start %= 360;
  int end = start + span;
  if (end <= 360) {
    tft.drawArc(CX, CY, 119, 115, start, end, color, kBg, false);
  } else {
    tft.drawArc(CX, CY, 119, 115, start, 360, color, kBg, false);
    if (end > 360) tft.drawArc(CX, CY, 119, 115, 0, end - 360, color, kBg, false);
  }
}

void haloFull(uint16_t color) {
  tft.drawArc(CX, CY, 119, 115, 0, 360, color, kBg, false);
}

void drawHalo(const Job&) {}

void serviceHalo() {}

void drawGauge(const Job& job) {
  tft.fillScreen(kBg);
  const char* state = prettyStatus(job.status);
  if (state && state[0] && uiTextWidth(state, false) <= 180) uiDrawCenter(state, CX, 52, kAccent, false);
  else if (job.status.length()) {
    String shown = job.status;
    shown.toUpperCase();
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(kAccent, kBg);
    if (tft.textWidth(shown, 2) <= 150) tft.drawString(shown, CX, 52, 2);
  }

  char pct[8];
  snprintf(pct, sizeof(pct), "%d", job.percent);
  int numW = tft.textWidth(pct, 6);
  int symW = tft.textWidth("%", 4);
  int left = CX - (numW + 8 + symW) / 2;
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(kInk, kBg);
  tft.drawString(pct, left, 108, 6);
  tft.setTextColor(kAccent, kBg);
  tft.drawString("%", left + numW + 8, 100, 4);

  rule(146, job.percent);

  tft.setTextDatum(MC_DATUM);
  int cur = job.layer;
  int total = job.layers;
  if (cur < 0 || cur > 9999) cur = 0;
  if (total < 0 || total > 9999) total = 0;
  char layer[24];
  snprintf(layer, sizeof(layer), "%d  /  %d", cur, total);
  tft.setTextColor(kMuted, kBg);
  tft.drawString(layer, CX, 170, 2);
  String name = baseName(job.file);
  if (name.length() && tft.textWidth(name, 2) <= 160) {
    tft.setTextColor(kInk, kBg);
    tft.drawString(name, CX, 190, 2);
  }
  pages(0, screenCount(job));
  linkLed(gLink);
}

void drawTemps(const Job& job) {
  tft.fillScreen(kBg);
  column(tr(MSG_UI_NOZZLE), job.nozzle, job.nozzleTarget, 72, kHot);
  column(tr(MSG_UI_BED), job.bed, job.bedTarget, 168, kBed);
  tft.drawFastVLine(CX, 88, 88, kTrack);
  pages(1, screenCount(job));
  linkLed(gLink);
}

void drawTimes(const Job& job) {
  char elapsed[48];
  char remain[48];
  formatHms(elapsed, sizeof(elapsed), job.elapsedSec);
  formatHms(remain, sizeof(remain), job.remainSec);
  tft.fillScreen(kBg);
  uiDrawCenter(tr(MSG_UI_ELAPSED), CX, 62, kMuted, false);
  uiDrawCenter(elapsed, CX, 94, kInk, uiTextWidth(elapsed, true) <= 200);
  tft.drawFastHLine(78, 122, 84, kTrack);
  uiDrawCenter(tr(MSG_UI_REMAIN), CX, 146, kAccent, false);
  uiDrawCenter(remain, CX, 176, kInk, uiTextWidth(remain, true) <= 200);
  pages(2, screenCount(job));
  linkLed(gLink);
}

void drawName(const String& name) {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, kBg);
  if (name.length() == 0) {
    uiDrawCenter(tr(MSG_UI_NO_FILE), CX, 186, kMuted, false);
    return;
  }
  if (tft.textWidth(name, 2) <= 150) {
    tft.drawString(name, CX, 186, 2);
    return;
  }
  int split = name.length() / 2;
  int space = name.lastIndexOf('_', split + 4);
  if (space < 4) space = name.lastIndexOf(' ', split + 4);
  if (space < 4) space = split;
  String a = name.substring(0, space);
  String b = name.substring(space + (name.charAt(space) == '_' || name.charAt(space) == ' ' ? 1 : 0));
  a.trim();
  b.trim();
  tft.drawString(a, CX, 178, 2);
  tft.drawString(b, CX, 194, 2);
}

void drawThumb(const Job& job) {
  tft.fillScreen(kBg);
  lockData();
  String shown = gThumbHeld && gImgFile.length() ? gImgFile : job.file;
  String name = baseName(shown);
  uint16_t* img = gImg;
  int iw = gImgW;
  int ih = gImgH;
  bool held = gThumbHeld && img && iw > 0 && ih > 0;
  unlockData();
  if (held) {
    int ox = (240 - iw) / 2;
    int oy = (168 - ih) / 2 + 16;
    if (oy < 16) oy = 16;
    tft.setSwapBytes(true);
    tft.pushImage(ox, oy, iw, ih, img);
    tft.setSwapBytes(false);
  } else {
    tft.setTextDatum(MC_DATUM);
    uiDrawCenter(tr(MSG_UI_NO_IMAGE), CX, 100, kMuted, false);
  }
  drawName(name);
  int page = showTimes(job) ? 3 : 2;
  pages(page, screenCount(job));
}

void drawPrinterOff() {
  tft.fillScreen(kBg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(kInk, kBg);
  tft.drawString("AD5X", CX, 46, 4);
  tft.drawString("Flashforge", CX, 74, 2);
  int x = CX - kForgeLogo / 2;
  int y = 92;
  tft.setSwapBytes(true);
  tft.pushImage(x, y, kForgeLogo, kForgeLogo, kForgeLogoPx);
  tft.setSwapBytes(false);
  uiDrawCenter(tr(MSG_UI_OFFLINE), CX, 188, kHot, false);
}

void drawMessage(const char* title, const char* sub) {
  tft.fillScreen(kBg);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(kInk, kBg);
  tft.drawString(title, CX, CY - 10, 4);
  uiDrawCenter(sub, CX, CY + 18, kMuted, false);
}

void show(const Job& job) {
  bool times = showTimes(job);
  int n = screenCount(job);
  if (gScreen >= n) gScreen = 0;
  if (gScreen == 0) {
    drawGauge(job);
    gShownKind = 0;
  } else if (gScreen == 1) {
    drawTemps(job);
    gShownKind = 1;
  } else if (times && gScreen == 2) {
    drawTimes(job);
    gShownKind = 2;
  } else {
    drawThumb(job);
    gShownKind = 3;
  }
  gScreen = (gScreen + 1) % n;
  gShownAt = millis();
}

const char* wifiStatusName(int status) {
  switch (status) {
    case WL_IDLE_STATUS: return tr(MSG_ST_IDLE);
    case WL_NO_SSID_AVAIL: return tr(MSG_ST_NO_SSID);
    case WL_SCAN_COMPLETED: return tr(MSG_ST_SCAN);
    case WL_CONNECTED: return tr(MSG_ST_CONNECTED);
    case WL_CONNECT_FAILED: return tr(MSG_ST_REFUSED);
    case WL_CONNECTION_LOST: return tr(MSG_ST_LOST);
    case WL_DISCONNECTED: return tr(MSG_ST_DISC);
    default: return tr(MSG_E_OTHER);
  }
}

const char* wifiReasonName(int reason) {
  switch (reason) {
    case 1: return tr(MSG_R_UNSPEC);
    case 2: return tr(MSG_R_AUTH_EXPIRE);
    case 8: return tr(MSG_R_LEAVE);
    case 15: return tr(MSG_R_HANDSHAKE);
    case 200: return tr(MSG_R_BEACON);
    case 201: return tr(MSG_R_NO_AP);
    case 202: return tr(MSG_R_BAD_PASS);
    case 203: return tr(MSG_R_ASSOC);
    case 204: return tr(MSG_R_HS_TIMEOUT);
    case 205: return tr(MSG_R_CONN_FAIL);
    case 39: return tr(MSG_R_TIMEOUT);
    default: return tr(MSG_E_OTHER);
  }
}

void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      logln(MSG_WIFI_RADIO);
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED: {
      auto& link = info.wifi_sta_connected;
      logf(MSG_WIFI_ASSOC, link.channel, link.authmode, link.bssid[0], link.bssid[1], link.bssid[2],
           link.bssid[3], link.bssid[4], link.bssid[5]);
#if WIFI_DHCP
      logln(MSG_WIFI_DHCP);
#else
      logf(MSG_WIFI_STATIC, WIFI_IP);
#endif
      gAssociated = true;
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      gHaveIp = true;
      logf(MSG_WIFI_IP, WiFi.localIP().toString().c_str(), WiFi.subnetMask().toString().c_str(),
           WiFi.gatewayIP().toString().c_str(), WiFi.RSSI());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      gHaveIp = false;
      gAssociated = false;
      auto& drop = info.wifi_sta_disconnected;
      logf(MSG_WIFI_REJECT, drop.bssid[0], drop.bssid[1], drop.bssid[2], drop.bssid[3], drop.bssid[4],
           drop.bssid[5], drop.reason, wifiReasonName(drop.reason));
      break;
    }
    default:
      break;
  }
}

void applyWifiAddress() {
#if WIFI_DHCP
  WiFi.config(IPAddress(), IPAddress(), IPAddress(), IPAddress());
#else
  IPAddress ip, gw, mask, dns;
  ip.fromString(WIFI_IP);
  gw.fromString(WIFI_GW);
  mask.fromString(WIFI_MASK);
  dns.fromString(WIFI_DNS);
  WiFi.config(ip, gw, mask, dns);
#endif
}

bool tryWifi(uint32_t timeoutMs) {
#if WIFI_DHCP
  logf(MSG_WIFI_BEGIN_DHCP, WIFI_SSID, WIFI_CHANNEL);
#else
  logf(MSG_WIFI_BEGIN_IP, WIFI_SSID, WIFI_CHANNEL, WIFI_IP);
#endif
  applyWifiAddress();
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  if (WIFI_CHANNEL > 0) WiFi.begin(WIFI_SSID, WIFI_PASS, WIFI_CHANNEL);
  else WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t start = millis();
  uint32_t logged = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    vTaskDelay(pdMS_TO_TICKS(20));
    if (logged == 0 || millis() - logged >= 1000) {
      logged = millis();
      int status = WiFi.status();
      logf(MSG_WIFI_WAIT, (unsigned long)((millis() - start) / 1000), status, wifiStatusName(status));
    }
  }
  return WiFi.status() == WL_CONNECTED;
}

void macText(const uint8_t* mac, char* out) {
  sprintf(out, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool roamIfBetter() {
  if (WiFi.status() != WL_CONNECTED || !WiFi.BSSID()) return false;
  uint8_t current[6];
  memcpy(current, WiFi.BSSID(), 6);
  int currentRssi = WiFi.RSSI();
  char curTxt[18];
  macText(current, curTxt);

  int found = WiFi.scanNetworks(false, false, false, 150, WIFI_CHANNEL);
  int bestRssi = -127;
  int bestChannel = WIFI_CHANNEL;
  uint8_t best[6] = {};
  bool have = false;
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) != WIFI_SSID) continue;
    int rssi = WiFi.RSSI(i);
    logf(MSG_WIFI_HEAR, WiFi.BSSIDstr(i).c_str(), rssi);
    if (rssi > bestRssi && WiFi.BSSID(i)) {
      bestRssi = rssi;
      bestChannel = WiFi.channel(i);
      memcpy(best, WiFi.BSSID(i), 6);
      have = true;
    }
  }
  WiFi.scanDelete();
  if (!have) {
    logf(MSG_WIFI_KEEP, curTxt, currentRssi);
    return false;
  }
  char bestTxt[18];
  macText(best, bestTxt);
  if (memcmp(best, current, 6) == 0 || bestRssi < currentRssi + 8) {
    logf(MSG_WIFI_KEEP_BEST, curTxt, currentRssi, bestTxt, bestRssi);
    return false;
  }
  logf(MSG_WIFI_ROAM, curTxt, currentRssi, bestTxt, bestRssi);
  applyWifiAddress();
  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(200));
  WiFi.begin(WIFI_SSID, WIFI_PASS, bestChannel, best);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (WiFi.status() == WL_CONNECTED) {
    logf(MSG_WIFI_ROAM_OK, WiFi.BSSIDstr().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
    return true;
  }
  logln(MSG_WIFI_ROAM_FAIL);
  return false;
}

void finishWifiOk() {
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(false);
  gHaveIp = true;
  configTime(-3 * 3600, 0, "a.ntp.br", "pool.ntp.org");
  logf(MSG_WIFI_OK, WiFi.localIP().toString().c_str(), WiFi.RSSI(), WiFi.channel());
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  if (WiFi.status() == WL_CONNECTED) {
    gWifiPhase = 2;
    finishWifiOk();
    return;
  }
  gWifiPhase = 0;
  WiFi.disconnect(false, false);
  vTaskDelay(pdMS_TO_TICKS(300));
  bool ok = tryWifi(45000);
  if (!ok) {
    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(1500));
    ok = tryWifi(45000);
  }
  if (!ok) {
    gHaveIp = false;
    gWifiPhase = 1;
    logf(MSG_WIFI_FAIL, WiFi.status(), wifiStatusName(WiFi.status()));
    return;
  }
  gWifiPhase = 2;
  finishWifiOk();
}

extern "C" void onPingEnd(esp_ping_handle_t, void* args) {
  if (args) xSemaphoreGive(static_cast<SemaphoreHandle_t>(args));
}

bool pingPrinter(uint32_t* rttMs) {
  if (rttMs) *rttMs = 0;
  ip_addr_t addr;
  if (!ipaddr_aton(PRINTER_IP, &addr)) {
    logln(MSG_PING_IP);
    return false;
  }
  if (!gPingDone) gPingDone = xSemaphoreCreateBinary();
  while (xSemaphoreTake(gPingDone, 0) == pdTRUE) {
  }

  esp_ping_config_t cfg = {};
  cfg.count = 2;
  cfg.interval_ms = 200;
  cfg.timeout_ms = 800;
  cfg.data_size = 32;
  cfg.ttl = 64;
  cfg.target_addr = addr;
  cfg.task_stack_size = 4096;
  cfg.task_prio = 2;

  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_end = onPingEnd;
  cbs.cb_args = gPingDone;

  esp_ping_handle_t hdl = nullptr;
  if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK || !hdl) {
    logln(MSG_PING_SESSION);
    return false;
  }
  if (esp_ping_start(hdl) != ESP_OK) {
    logln(MSG_PING_START);
    esp_ping_delete_session(hdl);
    return false;
  }
  if (xSemaphoreTake(gPingDone, pdMS_TO_TICKS(3000)) != pdTRUE) {
    logln(MSG_PING_STUCK);
    esp_ping_stop(hdl);
  }
  vTaskDelay(pdMS_TO_TICKS(50));
  uint32_t replies = 0;
  uint32_t gap = 0;
  esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &replies, sizeof(replies));
  esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap));
  esp_ping_delete_session(hdl);
  if (rttMs) *rttMs = gap;
  return replies > 0;
}

void pollBoot() {
  static bool down = false;
  static uint32_t last = 0;
  bool pressed = digitalRead(kBootPin) == LOW;
  if (pressed && !down && millis() - last > 250) {
    down = true;
    last = millis();
    gDebug = !gDebug;
    bool now = gDebug;
    gDebug = true;
    logln(now ? MSG_DEBUG_ON : MSG_DEBUG_OFF);
    gDebug = now;
  }
  if (!pressed) down = false;
}

void setup() {
  Serial.begin(115200);
  pinMode(kBootPin, INPUT_PULLUP);
  useLang(APP_LANG);
  gDebug = DEBUG_LOG;
  logln(MSG_BOOT);
  WiFi.onEvent(onWiFiEvent);
  backlight(255);
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(kBg);
  Job blank;
  blank.ok = true;
  blank.nozzle = blank.nozzleTarget = blank.bed = blank.bedTarget = -1;
  drawGauge(blank);
  drawHalo(blank);
  gShownAt = millis();
  gDataMu = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

void netTask(void*) {
  logf(MSG_NET_CORE, xPortGetCoreID());
  connectWifi();
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      gLink = false;
      vTaskDelay(pdMS_TO_TICKS(8000));
      connectWifi();
      continue;
    }
    if (WiFi.status() == WL_CONNECTED) {
      roamIfBetter();
      if (WiFi.status() != WL_CONNECTED) continue;
      uint32_t rtt = 0;
      bool alive = pingPrinter(&rtt);
      if (!alive) {
        lockData();
        gLink = false;
        if (!gPrinterOff) {
          gPrinterOff = true;
          gShownAt = 0;
        }
        unlockData();
        logln(MSG_PING_OFF);
        vTaskDelay(pdMS_TO_TICKS(kPingMs));
        continue;
      }
      lockData();
      bool back = gPrinterOff;
      gPrinterOff = false;
      if (back) gShownAt = 0;
      unlockData();
      logf(MSG_PING_OK, (unsigned long)rtt);

      static uint32_t nextHttp = 0;
      if (nextHttp != 0 && (int32_t)(millis() - nextHttp) < 0) {
        vTaskDelay(pdMS_TO_TICKS(kPingMs));
        continue;
      }
      Job job;
      bool ok = queryPrinter(job);
      String file;
      bool held = false;
      String imgName;
      lockData();
      if (ok) {
        if (!gHave || gPrinterMiss) gShownAt = 0;
        gPrinterMiss = 0;
        gLast = job;
        gHave = true;
        gLink = true;
      } else {
        gLink = false;
        if (gPrinterMiss < 1000) gPrinterMiss++;
        logf(MSG_NO_REPLY, gPrinterMiss);
      }
      file = gLast.file;
      held = gThumbHeld;
      imgName = gImgFile;
      unlockData();
      nextHttp = millis() + kPollMs;

      if (held && file.length() && baseName(file) != baseName(imgName)) {
        lockData();
        gThumbHeld = false;
        unlockData();
        held = false;
      }
      if (!held && file.length() &&
          (gThumbAttemptAt == 0 || millis() - gThumbAttemptAt >= 15000)) {
        gThumbAttemptAt = millis();
        loadThumb(file);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(kPingMs));
  }
}

void loop() {
  pollBoot();
  if (WiFi.status() != WL_CONNECTED) {
    gHaveIp = false;
    if (!gHave) {
      static int shownPhase = -1;
      int phase = gWifiPhase;
      if (phase != shownPhase) {
        shownPhase = phase;
        drawMessage("WIFI", phase == 1 ? tr(MSG_UI_NO_NET) : tr(MSG_UI_CONNECTING));
        gShownAt = millis();
      }
    } else {
      linkLed(false);
    }
    serviceHalo();
    delay(15);
    return;
  }
  gHaveIp = true;

  if (gPrinterOff) {
    if (gShownKind != 4) {
      drawPrinterOff();
      gShownKind = 4;
      gShownAt = millis();
    }
    delay(40);
    return;
  }

  Job snap;
  bool have;
  lockData();
  have = gHave;
  if (have) snap = gLast;
  unlockData();

  if (gThumbWipe) {
    gThumbWipe = false;
    if (gShownKind == 3) gShownAt = 0;
  }
  if (have && snap.status != "printing" && gShownKind == 2) gShownAt = 0;

  if (!have) {
    if (gShownAt == 0 || millis() - gShownAt >= 2000) {
      Job blank;
      blank.ok = true;
      blank.nozzle = blank.nozzleTarget = blank.bed = blank.bedTarget = -1;
      drawGauge(blank);
      gShownAt = millis();
      drawHalo(blank);
    }
    delay(40);
    return;
  }

  if (gShownAt == 0 || millis() - gShownAt >= kScreenMs) show(snap);
  delay(40);
}
