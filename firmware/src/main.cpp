// M5StopWatch voice chat client.
// Hold KEY A (yellow) to talk. A mic task records into PSRAM while an upload
// task streams the audio to the Mac relay and fetches the transcript so far;
// after release the reply is polled and typed out as it arrives. Drag the screen to scroll through
// the conversation. KEY B (blue) starts a new conversation; holding KEY B
// returns to the launcher menu (ota_0).
#include <M5Unified.h>
#include <esp_ota_ops.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>
#include "env_config.h"
#include "ble_link.h"
#include "font_noto.h"

#define LOGF(...) do { printf(__VA_ARGS__); fflush(stdout); } while (0)

static constexpr uint32_t SAMPLE_RATE = 16000;
static constexpr size_t CHUNK = 800;                     // 50 ms per record() call
static constexpr size_t MAX_SAMPLES = SAMPLE_RATE * 20;  // 20 s limit
static constexpr float MIN_SECONDS = 0.4f;
static constexpr size_t MAX_TURNS = 20;
static constexpr int TEXT_CANVAS_H = 4000;

// Screen flip: accelerometer axis that points "up" along the display.
// Check the device log ("accel ...") and adjust if the flip goes the wrong way.
static constexpr int FLIP_AXIS = 0;        // 0:x 1:y 2:z (x verified on the device)
static constexpr float FLIP_SIGN = -1.0f;  // verified on the device
static constexpr float FLIP_THRESHOLD = 0.45f;  // g; between -T and T keeps the current side

enum class St { Idle, Recording, Sending, Thinking };
static St st = St::Idle;  // the eyes show from the start; Wi-Fi connects in the background

struct Turn { String heard, reply; };
static std::vector<Turn> turns;
static Turn pending;
static bool hasPending = false;
static String notice;  // one-line message shown under the history (errors etc.)

static int16_t* rec_buf;
static volatile size_t rec_len = 0;   // samples queued to the mic (written by micTask)
static volatile int level = 0;
static volatile bool micOn = false;   // main -> micTask: keep recording
static volatile bool micIdle = true;  // micTask -> main: all queued samples are filled
static String jobId;
static uint32_t lastPollMs = 0;

// upload task <-> main
static SemaphoreHandle_t upMutex;
static volatile bool upStart = false, upFinish = false, upDone = false;
static volatile size_t upFinalLen = 0;
static String upPartial;          // transcript so far (guarded by upMutex)
static String upJobId, upText;    // result of /stream/end (valid when upDone)
static int upCode = 0;
static volatile bool upStartFailed = false;  // the relay did not answer /stream/start
static volatile bool upBle = false;          // this recording went over BLE (set by uploadTask)
static constexpr uint32_t BLE_HEARD_TIMEOUT_MS = 20000;  // END -> HEARD (transcription on the Mac)
static constexpr uint32_t BLE_LOST_MS = 15000;           // link down this long while thinking: give up

// Shown only when the owner tries to talk and the watch cannot reach the outside:
// "x" eyes and a one-line reason while the yellow key is held, and a moment after.
static uint32_t offlineUntil = 0;
static const char* offlineMsg = "";
static constexpr uint32_t OFFLINE_SHOW_MS = 2500;
static void showOffline(const char* msg, uint32_t now) { offlineMsg = msg; offlineUntil = now + OFFLINE_SHOW_MS; }
static bool historyLoaded = false;
static bool wifiUp() { return WiFi.status() == WL_CONNECTED; }
static constexpr uint32_t TAP_MS = 400;  // a yellow-key press shorter than this is a tap, not talk

// typewriter for the reply
static String replyTarget;        // everything received so far
static size_t replyShown = 0;     // bytes of replyTarget on screen
static bool replyFinished = false, replyOk = true;
static uint32_t lastTypeMs = 0;
static constexpr float TYPE_CHARS_PER_SEC = 40.0f;

static String getPartial() {
  xSemaphoreTake(upMutex, portMAX_DELAY);
  String p = upPartial;
  xSemaphoreGive(upMutex);
  return p;
}

static M5Canvas screen(&M5.Display);
static M5Canvas textCv(&screen);
static int W, H, CX, CY, TW, TXL;
static int textH = 0, scrollY = 0, lastTouchY = -1;
static bool textDirty = true, followTail = true;

static const uint16_t COL_HEARD = 0x9D3F;   // light blue: what the owner said
static const uint16_t COL_HINT = 0x7BEF;
static int rotation = 0;

// Idle screen shows the eyes; the conversation text is shown while a reply is
// streaming, for a while after it, or when the owner swipes up/down on the eyes.
static bool showHistory = false;
static uint32_t historyUntil = 0;
static constexpr uint32_t HISTORY_SHOW_MS = 15000;
// button tips: shown near each physical button for a while after a click on the power key
static uint32_t tipAUntil = 0, tipBUntil = 0;
static constexpr uint32_t TIP_MS = 2700;

// ---------- drawing ----------
// Chat layout: the owner's words right-aligned, the avatar's replies left-aligned.
// Wrapping is done here (not by the canvas) so each line can be measured and aligned.
static constexpr float BUBBLE_WIDTH = 0.85f;  // max line width as a share of the text area

static int lineWidth(const String& line);

static void wrapLines(const String& s, int maxW, std::vector<String>& out) {
  String line;
  size_t i = 0;
  while (i < s.length()) {
    size_t j = i + 1;
    while (j < s.length() && (s[j] & 0xC0) == 0x80) ++j;  // one UTF-8 character
    String ch = s.substring(i, j);
    i = j;
    if (ch == "\n") { out.push_back(line); line = ""; continue; }
    if (line.length() && lineWidth(line + ch) > maxW) { out.push_back(line); line = ""; }
    line += ch;
  }
  if (line.length() || out.empty()) out.push_back(line);
}

// Characters missing from the Noto subset are drawn with M5GFX's built-in font
// (JIS level 1 and 2). That font lives in its own small sprite so the Noto font on
// textCv never has to be reloaded; each character is drawn there and pasted over.
static M5Canvas fallbackCv(&textCv);

// The VLW data starts with a glyph table sorted by code point (24-byte header,
// then 28 bytes per glyph whose first field is the big-endian code point).
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static bool inNoto(const String& ch) {
  if (ch.length() == 1) return true;  // ASCII is always in the subset
  const uint8_t* u = (const uint8_t*)ch.c_str();
  uint32_t cp = ch.length() == 2 ? (u[0] & 0x1F) << 6 | (u[1] & 0x3F)
              : ch.length() == 3 ? (u[0] & 0x0F) << 12 | (u[1] & 0x3F) << 6 | (u[2] & 0x3F)
              : 0x10000;  // 4-byte characters are outside the 16-bit table
  int lo = 0, hi = (int)be32(FONT_NOTO) - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint32_t v = be32(FONT_NOTO + 24 + 28 * mid);
    if (v == cp) return true;
    if (v < cp) lo = mid + 1; else hi = mid - 1;
  }
  return false;
}

static int charWidth(const String& ch) {
  return inNoto(ch) ? textCv.textWidth(ch) : fallbackCv.textWidth(ch);
}

static int lineWidth(const String& line) {
  int w = 0;
  for (size_t i = 0; i < line.length();) {
    size_t j = i + 1;
    while (j < line.length() && (line[j] & 0xC0) == 0x80) ++j;
    w += charWidth(line.substring(i, j));
    i = j;
  }
  return w;
}

static void drawLine(const String& line, int x, int y, uint16_t color) {
  String run;  // consecutive characters the Noto subset has
  auto flush = [&]() { if (run.length()) { textCv.drawString(run, x, y); x += textCv.textWidth(run); run = ""; } };
  for (size_t i = 0; i < line.length();) {
    size_t j = i + 1;
    while (j < line.length() && (line[j] & 0xC0) == 0x80) ++j;
    String ch = line.substring(i, j);
    i = j;
    if (inNoto(ch)) { run += ch; continue; }
    flush();
    fallbackCv.fillScreen(TFT_BLACK);
    fallbackCv.setTextColor(color);
    fallbackCv.drawString(ch, 0, 0);
    fallbackCv.pushSprite(x, y + 2, TFT_BLACK);
    x += fallbackCv.textWidth(ch);
  }
  flush();
}

static int drawBlock(const String& text, bool right, uint16_t color, int y) {
  std::vector<String> lines;
  wrapLines(text, (int)(TW * BUBBLE_WIDTH), lines);
  int lh = textCv.fontHeight() + 4;
  textCv.setTextColor(color);
  textCv.setTextDatum(textdatum_t::top_left);
  for (auto& l : lines) {
    int x = right ? TW - lineWidth(l) : 0;
    drawLine(l, x, y, color);
    y += lh;
  }
  return y;
}

static int printTurn(const Turn& t, bool replyPending, int y) {
  if (t.heard.length()) y = drawBlock(t.heard, true, COL_HEARD, y) + 6;
  String r = t.reply.length() ? t.reply : (replyPending ? String("…") : String(""));
  if (r.length()) y = drawBlock(r, false, TFT_WHITE, y);
  return y + 22;  // gap between turns
}

static void layoutText() {
  for (;;) {  // drop the oldest turns until everything fits in the canvas
    textCv.fillScreen(TFT_BLACK);
    int y = 0;
    for (auto& t : turns) y = printTurn(t, false, y);
    if (hasPending) y = printTurn({pending.heard, replyTarget.substring(0, replyShown)}, true, y);
    if (notice.length()) y = drawBlock(notice, false, 0xFBEF, y) + 22;
    if (y + 170 < TEXT_CANVAS_H || turns.empty()) {
      textH = y;
      break;
    }
    turns.erase(turns.begin());
  }
  textDirty = false;
}

// ---------- eyes ----------
// Two white rounded eyes. They blink, glance around on their own, and look at the
// finger while the screen is touched. Gaze is in -1..1 on both axes.
struct Eyes {
  float x = 0, y = 0;    // current gaze
  float tx = 0, ty = 0;  // target gaze
  uint32_t nextGlanceMs = 2000, nextBlinkMs = 3000, blinkStartMs = 0;
  bool followingTouch = false;
  // sleep: after SLEEP_AFTER_MS without any activity the eyes close to a line, and
  // they wake up by themselves after a while or on a touch / button press
  bool asleep = false;
  float sleepAmt = 0;  // 0 awake .. 1 eyes closed (eased)
  uint32_t lastActivityMs = 0, wakeAtMs = 0;
};
static Eyes eyes;
static constexpr int EYE_W = 78, EYE_H = 118, EYE_GAP = 150, EYE_R = 37;  // corner radius (39 = fully round ends)
static constexpr int EYE_LOOK_X = 110, EYE_LOOK_Y = 100;  // px at full gaze (touching the edge brings the eyes to the rim)

// Expressions triggered by the sensors: a single knock surprises her, shaking makes
// her angry, drawing circles on the screen makes her dizzy, stroking the upper part of the screen makes
// her smile. A low battery makes her tired while it lasts.
enum class Mood { Normal, Angry, Happy, Dizzy, Surprised };
static Mood mood = Mood::Normal;
static uint32_t moodUntil = 0;
static constexpr uint32_t MOOD_MS = 3000, SURPRISE_MS = 1500;
static bool tired = false;  // battery <= 20 % and not charging

static void setMood(Mood m, uint32_t now, uint32_t ms = MOOD_MS) { mood = m; moodUntil = now + ms; }

static constexpr uint32_t SLEEP_AFTER_MS = 30000;
static constexpr uint32_t SLEEP_MIN_MS = 60000, SLEEP_MAX_MS = 600000;  // how long she sleeps (random)

static void wakeEyes(uint32_t now) {
  eyes.lastActivityMs = now;
  if (!eyes.asleep) return;
  eyes.asleep = false;
  eyes.nextBlinkMs = now + 500;  // a sleepy blink right after waking
}

static void lookAt(float x, float y) { eyes.tx = constrain(x, -1.0f, 1.0f); eyes.ty = constrain(y, -1.0f, 1.0f); }

static void updateEyes(uint32_t now) {
  if (st != St::Idle || showHistory) eyes.lastActivityMs = now;  // only the idle eyes fall asleep
  if (!eyes.asleep && now - eyes.lastActivityMs > SLEEP_AFTER_MS) {
    eyes.asleep = true;
    eyes.wakeAtMs = now + SLEEP_MIN_MS + random(SLEEP_MAX_MS - SLEEP_MIN_MS);
    lookAt(0, 0.35f);  // head slightly down
  }
  if (eyes.asleep && now > eyes.wakeAtMs) wakeEyes(now);
  eyes.sleepAmt += ((eyes.asleep ? 1.0f : 0.0f) - eyes.sleepAmt) * (eyes.asleep ? 0.04f : 0.15f);
  if (eyes.asleep) {
    eyes.x += (eyes.tx - eyes.x) * 0.05f;
    eyes.y += (eyes.ty - eyes.y) * 0.05f;
    return;
  }
  if (!eyes.followingTouch && now > eyes.nextGlanceMs) {
    // idle glances: mostly near the centre, sometimes further out; tired: low and slow
    bool far = random(4) == 0;
    float gx = random(-100, 101) / (far ? 100.0f : 300.0f), gy = random(-100, 101) / (far ? 150.0f : 400.0f);
    if (tired) { gx *= 0.4f; gy = 0.35f + gy * 0.2f; }
    lookAt(gx, gy);
    eyes.nextGlanceMs = now + (tired ? 3000 + random(4000) : 1200 + random(2800));
  }
  if (now > eyes.nextBlinkMs && !eyes.blinkStartMs) eyes.blinkStartMs = now;
  if (eyes.blinkStartMs && now - eyes.blinkStartMs > 170) {
    eyes.blinkStartMs = 0;
    eyes.nextBlinkMs = now + 2200 + random(3500) - (random(5) == 0 ? 2000 : 0);  // sometimes a quick double blink
  }
  float k = eyes.followingTouch ? 0.35f : (tired ? 0.06f : 0.18f);
  eyes.x += (eyes.tx - eyes.x) * k;
  eyes.y += (eyes.ty - eyes.y) * k;
}

// scale: 1 = full size
static void drawEyes(int cx, int cy, float scale) {
  uint32_t now = millis();
  float open = 1.0f;
  if (eyes.blinkStartMs) {
    float u = (now - eyes.blinkStartMs) / 170.0f;
    open = fabsf(u * 2 - 1);  // 1 -> 0 -> 1
  }
  open *= 1 - eyes.sleepAmt;
  Mood m = (now < moodUntil && scale >= 1.0f) ? mood : Mood::Normal;
  float breath = eyes.sleepAmt * sinf(now / 900.0f) * 4 * scale;  // slow breathing while asleep
  float ox = eyes.x * EYE_LOOK_X * scale, oy = eyes.y * EYE_LOOK_Y * scale + breath;
  if (m == Mood::Angry) { ox += random(-3, 4); oy += random(-3, 4); }  // trembling
  if (m == Mood::Dizzy) { ox = cosf(now / 160.0f) * 14 * scale; oy = sinf(now / 160.0f) * 10 * scale; }  // wobbling
  if (m == Mood::Happy) oy += sinf(now / 700.0f) * 3 * scale;          // a happy little bounce
  for (int side = -1; side <= 1; side += 2) {
    // turning towards one side moves that eye to the far side of the head: it gets
    // a little smaller and the near eye a little bigger (perspective)
    float persp = 1 - side * eyes.x * 0.15f;
    float tilt = 1 - fabsf(eyes.y) * 0.15f;  // looking up or down: seen at an angle, a little flatter
    float ew = EYE_W * scale * persp;
    float eh = fmaxf(EYE_H * scale * persp * tilt * open, 8 * scale);
    float ex = cx + side * EYE_GAP * scale / 2 + ox, ey = cy + oy;
    if (millis() < offlineUntil && scale >= 1.0f) {
      // "x" eyes: could not reach the outside
      float d = ew * 0.42f;
      screen.drawWideLine(ex - d, ey - d, ex + d, ey + d, 14 * scale, TFT_WHITE);
      screen.drawWideLine(ex - d, ey + d, ex + d, ey - d, 14 * scale, TFT_WHITE);
      continue;
    }
    if (m == Mood::Dizzy) {
      // spiral eyes: an Archimedean spiral drawn as closely spaced dots so the line
      // keeps one smooth width, slowly turning (both eyes the same shape and direction)
      const float turns = 2.5f, r0 = 4 * scale, r1 = 52 * scale, dot = 4.2f * scale;
      float phase = now / 260.0f;
      float len = PI * turns * (r0 + r1);  // approximate length of the curve
      int n = (int)(len / (1.4f * scale));
      for (int i = 0; i <= n; ++i) {
        float t = (float)i / n, a = t * turns * 2 * PI + phase, r = r0 + (r1 - r0) * t;
        screen.fillSmoothCircle(ex + cosf(a) * r, ey + sinf(a) * r, dot, TFT_WHITE);
      }
      continue;
    }
    if (m == Mood::Surprised) { ew *= 1.15f; eh = fmaxf(EYE_H * scale * 1.22f * open, 8 * scale); }  // wide open
    screen.fillSmoothRoundRect(ex - ew / 2, ey - eh / 2, ew, eh, fminf(EYE_R * scale * (m == Mood::Surprised ? 1.3f : 1), eh / 2), TFT_WHITE);
    if (tired && m == Mood::Normal && open > 0.3f) {
      // heavy upper lids: the top 45 % of the eye is covered, the lid line droops outwards
      float lid = ey - eh / 2 + eh * 0.45f;
      screen.fillRect(ex - ew / 2 - 2, ey - eh / 2 - 4, ew + 4, lid - (ey - eh / 2) + 4, TFT_BLACK);
      screen.fillTriangle(ex - side * ew / 2, lid - 1, ex + side * (ew / 2 + 3), lid - 1, ex + side * (ew / 2 + 3), lid + eh * 0.08f, TFT_BLACK);
    }
    float inner = ex - side * ew / 2, outer = ex + side * ew / 2, top = ey - eh / 2 - 2;
    if (m == Mood::Angry) {
      // cut the inner upper corner away: slanted, glaring eyes
      screen.fillTriangle(outer + side * 4, top - 4, inner - side * 4, top - 4, inner - side * 4, ey - eh * 0.05f, TFT_BLACK);
    } else if (m == Mood::Happy) {
      // the cheek pushes the lower lid up (a big circle cuts the bottom of the eye),
      // plus a soft pink blush under the outer side of the eye
      float bottom = ey + eh / 2, R = ew * 1.35f;
      screen.fillCircle(ex, bottom - EYE_H * scale * 0.42f + R, R, TFT_BLACK);
      screen.fillEllipse(ex + side * 22 * scale, bottom + 6 * scale, 26 * scale, 14 * scale, screen.color565(140, 66, 82));
    }
  }
  if (m == Mood::Dizzy) {
    // three chicks circling above the head; the ones at the back are smaller and dimmer
    for (int i = 0; i < 3; ++i) {
      float a = now / 450.0f + i * 2 * PI / 3;
      float depth = (sinf(a) + 1) / 2;  // 0 back .. 1 front
      float x = cx + cosf(a) * 120 * scale, y = cy - 118 * scale + sinf(a) * 22 * scale;
      float r = (13 + 5 * depth) * scale;
      uint8_t k = 200 + (uint8_t)(55 * depth);
      uint16_t body = screen.color565(k, k * 210 / 255, 40 * k / 255), beak = screen.color565(k, k * 120 / 255, 0);
      bool right = cosf(a + PI / 2) > 0;  // facing the direction of travel
      screen.fillSmoothCircle(x, y, r, body);
      screen.fillSmoothCircle(x + (right ? 0.45f : -0.45f) * r, y - r * 0.55f, r * 0.62f, body);  // head
      screen.fillTriangle(x + (right ? 1.0f : -1.0f) * r, y - r * 0.6f, x + (right ? 1.45f : -1.45f) * r, y - r * 0.45f,
                          x + (right ? 1.0f : -1.0f) * r, y - r * 0.3f, beak);
      screen.fillSmoothCircle(x + (right ? 0.62f : -0.62f) * r, y - r * 0.7f, fmaxf(1.5f, r * 0.12f), TFT_BLACK);
    }
  }
  if (eyes.sleepAmt > 0.9f && scale >= 1.0f) {
    // "Z" letters drifting up to the upper right
    screen.setTextDatum(textdatum_t::middle_center);
    for (int i = 0; i < 3; ++i) {
      float u = fmodf(now / 4500.0f + i / 3.0f, 1.0f);
      uint8_t g = (uint8_t)(200 * (1 - u));
      screen.setTextColor(screen.color565(g, g, g));
      screen.setTextSize(1.5f + u * 1.3f);  // grows as it rises
      screen.drawString("Z", cx + EYE_GAP / 2 + 50 + u * 70, cy - 70 - u * 110);
    }
    screen.setTextSize(1);
  }
}

// Shake: three sharp changes of acceleration within 0.8 s.
static void detectShake(uint32_t now) {
  static uint32_t peaks[3] = {};
  static int n = 0;
  static uint32_t lastPeak = 0;
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  float g = sqrtf(ax * ax + ay * ay + az * az);
  if (fabsf(g - 1.0f) > 1.2f && now - lastPeak > 80) {
    lastPeak = now;
    peaks[n++ % 3] = now;
    LOGF("accel peak %.2f g\n", g);
    if (n >= 3 && now - peaks[n % 3] < 800) { setMood(Mood::Angry, now); wakeEyes(now); n = 0; }
    else if (!(now < moodUntil && mood == Mood::Angry)) { setMood(Mood::Surprised, now, SURPRISE_MS); wakeEyes(now); }
  }
}

// Circles: the finger going round the centre of the screen. The angle is summed while
// the finger stays down; two full turns make her dizzy.
static void detectCircles(const m5::touch_detail_t& t, uint32_t now) {
  static float lastA = 0, total = 0;
  static bool tracking = false;
  float dx = t.x - CX, dy = t.y - CY;
  if (!t.isPressed() || dx * dx + dy * dy < 50 * 50) { tracking = false; total = 0; return; }
  float a = atan2f(dy, dx);
  if (tracking) {
    float d = a - lastA;
    if (d > PI) d -= 2 * PI;
    if (d < -PI) d += 2 * PI;
    total += d;
    if (fabsf(total) > 4 * PI && !(now < moodUntil && mood == Mood::Dizzy)) {
      setMood(Mood::Dizzy, now); total = 0;
    }
  }
  lastA = a;
  tracking = true;
}

// Battery: checked every 30 s.
static void updateBattery(uint32_t now) {
  static uint32_t last = 0;
  if (last && now - last < 30000) return;
  last = now;
  int level = M5.Power.getBatteryLevel();
  bool charging = M5.Power.isCharging();
  bool t = level >= 0 && level <= 20 && !charging;
  static bool logged = false;
  if (t != tired || !logged) LOGF("battery %d%% charging %d -> tired %d\n", level, charging, t);
  logged = true;
  tired = t;
}

// Petting: the finger goes left-right-left (two direction changes) in the upper
// part of the screen within 1.5 s.
static void detectPetting(const m5::touch_detail_t& t, uint32_t now) {
  static int lastX = -1, dir = 0, turns = 0, minY = 0, maxY = 0;
  static uint32_t firstTurn = 0;
  if (!t.isPressed() || t.y > H * 0.4f) { lastX = -1; dir = 0; turns = 0; return; }
  if (lastX < 0) { minY = maxY = t.y; }
  minY = min(minY, (int)t.y); maxY = max(maxY, (int)t.y);
  if (maxY - minY > 60) { turns = 0; return; }  // going round, not stroking
  if (lastX >= 0 && abs(t.x - lastX) > 6) {
    int d = t.x > lastX ? 1 : -1;
    if (dir && d != dir) {
      if (turns == 0) firstTurn = now;
      if (++turns >= 2 && now - firstTurn < 1500) { setMood(Mood::Happy, now); turns = 0; }
    }
    dir = d;
    lastX = t.x;
  } else if (lastX < 0) {
    lastX = t.x;
  }
}

// Tip near a physical button: yellow (A) top-left, blue (B) top-right with the
// screen upright; bottom-right / bottom-left when the screen is flipped.
static void drawTip(bool buttonA, const char* line1, const char* line2, uint16_t color, int shift = 0) {
  bool flipped = rotation == 2;
  bool left = buttonA != flipped;
  // yellow on the left, blue on the right, side by side at the same height; low
  // enough that the circle is wide there, above the idle eyes
  int y = flipped ? H - 96 - 54 - shift : 96 + shift;
  int x = left ? 52 : W - 52;
  screen.setTextDatum(left ? textdatum_t::top_left : textdatum_t::top_right);
  screen.setTextColor(color);
  screen.drawString(line1, x, y);
  screen.drawString(line2, x, y + 30);
  screen.setTextDatum(textdatum_t::middle_center);
}

// thin voice level bar under the eyes while listening
static void drawLevel(int cy) {
  int w = 160, h = 6, x = CX - w / 2;
  screen.fillRoundRect(x, cy, w, h, 3, 0x2104);
  screen.fillRoundRect(x, cy, w * level / 40, h, 3, 0xFE60);
}

static void drawHistory() {
  if (textDirty) layoutText();
  const int top = 80, bottom = H - 80, viewH = bottom - top;
  if (textH <= viewH) {
    textCv.pushSprite(TXL, top + (viewH - textH) / 2);  // short content: centered
  } else {
    int maxScroll = textH - viewH;
    if (followTail) scrollY = maxScroll;
    scrollY = constrain(scrollY, 0, maxScroll);
    screen.setClipRect(TXL, top, TW, viewH);
    textCv.pushSprite(TXL, top - scrollY);
    screen.clearClipRect();
    // scroll position dot along the right edge of the circle
    float f = scrollY / (float)maxScroll;
    float a = (-40 + 80 * f) * DEG_TO_RAD;
    screen.fillCircle(CX + cosf(a) * (W / 2 - 14), CY + sinf(a) * (W / 2 - 14), 5, COL_HINT);
  }
  drawEyes(CX, 44, 0.2f);  // small eyes on top of the text
}

static void render() {
  screen.fillScreen(TFT_BLACK);
  screen.setTextDatum(textdatum_t::middle_center);
  updateEyes(millis());

  if (millis() < offlineUntil) {
    drawEyes(CX, CY - 20, 1.0f);
    screen.setTextColor(0xFBEF);
    screen.drawString(offlineMsg, CX, H - 110);
  } else if (st == St::Recording || st == St::Sending || (st == St::Thinking && replyShown == 0)) {
    int ey = CY - 45;
    if (st == St::Thinking) lookAt(0.5f, -0.8f);  // looking up while thinking
    drawEyes(CX, ey, 0.85f);
    if (st == St::Recording) drawLevel(ey + EYE_H / 2 + 6);
    int ty = ey + EYE_H / 2 + 50;
    screen.setTextColor(TFT_WHITE);
    screen.drawString(st == St::Thinking ? "考え中…" : "聞き取り中", CX, ty);
    String heard = st == St::Thinking ? pending.heard : getPartial();
    if (heard.length()) {
      // live transcript, wrapped inside the lower part of the circle
      screen.setTextColor(COL_HEARD);
      screen.setTextDatum(textdatum_t::top_left);
      screen.setClipRect(TXL + 20, ty + 22, TW - 40, H - (ty + 22) - 30);
      screen.setTextWrap(true, false);
      screen.setCursor(TXL + 20, ty + 22);
      screen.print(heard);
      screen.clearClipRect();
      screen.setTextDatum(textdatum_t::middle_center);
    }
  } else if (st == St::Thinking || showHistory) {
    drawHistory();
  } else {
    drawEyes(CX, CY, 1.0f);
  }
  uint32_t now = millis();
  bool tipA = now < tipAUntil;
  bool tipB = now < tipBUntil;
  if (tipA) drawTip(true, "Hold: Talk", "Tap: History", 0xFE60);
  if (tipB) drawTip(false, "Tap: New session", "Hold: Menu", 0x5DDF);
  screen.pushSprite(0, 0);
}

// ---------- orientation ----------
// Flip 180 degrees so the text is upright. The gravity component along the
// display's vertical axis decides; it must stay past the threshold for
// FLIP_HOLD_MS, and nothing changes while the screen is close to horizontal.
static constexpr uint32_t FLIP_HOLD_MS = 500;
static constexpr float FLAT_Z = 0.85f;  // |z| above this: screen faces up/down, keep rotation

void updateRotation() {
  M5.Imu.update();
  float a[3];
  if (!M5.Imu.getAccel(&a[0], &a[1], &a[2])) return;
  float v = a[FLIP_AXIS] * FLIP_SIGN;
  int want = rotation;
  if (fabsf(a[2]) < FLAT_Z) {
    if (v > FLIP_THRESHOLD) want = 0;
    else if (v < -FLIP_THRESHOLD) want = 2;
  }
  static uint32_t lastLog = 0, candidateSince = 0;
  static int candidate = -1;
  uint32_t now = millis();
  if (now - lastLog > 500) {
    lastLog = now;
    LOGF("accel %.2f %.2f %.2f rot %d\n", a[0], a[1], a[2], rotation);
  }
  if (want == rotation) { candidate = -1; return; }
  if (want != candidate) { candidate = want; candidateSince = now; return; }
  if (now - candidateSince < FLIP_HOLD_MS) return;
  rotation = want; candidate = -1;
  M5.Display.setRotation(rotation);
  LOGF("rotation -> %d\n", rotation);
}

// ---------- network ----------
static void addHeaders(HTTPClient& http) { http.addHeader("X-Token", RELAY_TOKEN); }

static void showNotice(const String& m) { notice = m; textDirty = true; followTail = true; }

// HTTP when Wi-Fi is up, BLE when it is not (or the relay does not answer over HTTP).
static void fetchHistory() {
  String body;
  bool ok = false;
  if (wifiUp()) {
    HTTPClient http;
    http.begin(String(RELAY_URL) + "/history");
    http.setConnectTimeout(1500);
    http.setTimeout(5000);
    addHeaders(http);
    if (http.GET() == 200) { body = http.getString(); ok = true; }
    http.end();
  }
  if (!ok && bleReady()) ok = bleRequest(BLE_HISTORY_REQ, BLE_HISTORY, body, 5000);
  if (ok) {
    JsonDocument doc;
    if (!deserializeJson(doc, body)) {
      turns.clear();
      for (JsonObject it : doc["items"].as<JsonArray>()) {
        turns.push_back({it["heard"] | "", it["reply"] | ""});
      }
      LOGF("history: %u turns\n", (unsigned)turns.size());
    }
    historyLoaded = true;
  }  // on failure: nothing is shown, the loop tries again later
  textDirty = true; followTail = true;
}

static void takeBlePartial() {
  String p;
  if (!bleTake(BLE_PARTIAL, p)) return;
  JsonDocument doc;
  if (deserializeJson(doc, p)) return;
  xSemaphoreTake(upMutex, portMAX_DELAY);
  upPartial = doc["text"] | "";
  xSemaphoreGive(upMutex);
}

// BLE version of the streaming upload below: same inputs and results (upPartial,
// upJobId / upText / upCode, upStartFailed), audio as IMA ADPCM every 200 ms.
static void uploadBle() {
  size_t sent = 0;
  bool ok = bleStartAudio();
  if (!ok) upStartFailed = true;
  while (!upFinish) {
    vTaskDelay(pdMS_TO_TICKS(200));
    takeBlePartial();
    if (!ok || upFinish) continue;
    // the two most recent chunks may still be filling in the mic DMA queue
    size_t safe = rec_len > 2 * CHUNK ? (rec_len - 2 * CHUNK) & ~(size_t)1 : 0;
    if (safe <= sent) continue;
    if (bleSendAudio(&rec_buf[sent], safe - sent)) sent = safe;
    else { ok = false; upStartFailed = true; }  // link lost: "x" eyes like an unreachable relay
  }
  upJobId = ""; upText = ""; upCode = -1;
  if (!ok) {
    // nothing more to send; the Mac drops the unfinished recording
  } else if (upFinalLen < SAMPLE_RATE * MIN_SECONDS) {
    bleSend(BLE_CANCEL);
    upCode = 200;  // too short to be speech
  } else {
    size_t fin = (upFinalLen > sent ? upFinalLen : sent) & ~(size_t)1;
    if (bleSendAudio(&rec_buf[sent], fin - sent) && bleSend(BLE_END)) {
      String heard;
      for (uint32_t t0 = millis(); millis() - t0 < BLE_HEARD_TIMEOUT_MS && bleReady();) {
        takeBlePartial();
        if (bleTake(BLE_HEARD, heard)) {
          JsonDocument doc;
          if (!deserializeJson(doc, heard) && !doc["error"].is<const char*>()) {
            upJobId = doc["id"] | "";
            upText = doc["text"] | "";
            upCode = 200;
          } else {
            upCode = 500;
          }
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
      }
    }
  }
  LOGF("ble upload: code %d id '%s'\n", upCode, upJobId.c_str());
  upDone = true;
}

// Upload task: streams the recording while it is being made.
static void uploadTask(void*) {
  HTTPClient http;
  http.setReuse(true);
  auto post = [&](const String& path, const uint8_t* data, size_t len, String* body) -> int {
    http.begin(String(RELAY_URL) + path);
    http.setConnectTimeout(1500);  // an unreachable Mac should fail fast
    http.setTimeout(10000);
    addHeaders(http);
    http.addHeader("X-Sample-Rate", String(SAMPLE_RATE));
    http.addHeader("Content-Type", "application/octet-stream");
    int code = http.sendRequest("POST", (uint8_t*)data, len);
    if (body) *body = code > 0 ? http.getString() : String();
    http.end();
    return code;
  };
  for (;;) {
    if (!upStart) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    upStart = false;
    // transport: HTTP with Wi-Fi; BLE without Wi-Fi or when /stream/start fails
    upBle = !wifiUp() && bleReady();
    String body, sid;
    size_t sent = 0;
    int code = -1;
    if (!upBle) {
      code = post("/stream/start", nullptr, 0, &body);
      if (code == 200) {
        JsonDocument doc;
        deserializeJson(doc, body);
        sid = doc["sid"] | "";
      }
      if (sid.isEmpty() && bleReady()) { LOGF("stream/start failed (%d): using BLE\n", code); upBle = true; }
    }
    if (upBle) { uploadBle(); continue; }
    if (sid.isEmpty()) upStartFailed = true;
    while (!upFinish) {
      vTaskDelay(pdMS_TO_TICKS(400));
      // the two most recent chunks may still be filling in the mic DMA queue
      size_t safe = rec_len > 2 * CHUNK ? rec_len - 2 * CHUNK : 0;
      if (sid.isEmpty() || upFinish || safe <= sent) continue;
      if (post("/stream/chunk?sid=" + sid, (uint8_t*)&rec_buf[sent], (safe - sent) * 2, &body) == 200) {
        sent = safe;
        JsonDocument doc;
        deserializeJson(doc, body);
        xSemaphoreTake(upMutex, portMAX_DELAY);
        upPartial = doc["text"] | "";
        xSemaphoreGive(upMutex);
      }
    }
    // finish: main has stopped the mic and set upFinalLen
    upJobId = ""; upText = "";
    if (sid.isEmpty()) {
      upCode = code;
    } else if (upFinalLen < SAMPLE_RATE * MIN_SECONDS) {
      upCode = 200;  // too short to be speech: never reaches Claude (stream expires on the relay)
    } else {
      size_t fin = upFinalLen > sent ? upFinalLen : sent;
      upCode = post("/stream/end?sid=" + sid, (uint8_t*)&rec_buf[sent], (fin - sent) * 2, &body);
      if (upCode == 200) {
        JsonDocument doc;
        deserializeJson(doc, body);
        upJobId = doc["id"] | "";
        upText = doc["text"] | "";
      }
    }
    upDone = true;
  }
}

// Mic task: keeps the mic DMA queue fed regardless of how long drawing takes.
static void micTask(void*) {
  for (;;) {
    if (!micOn) {
      if (!micIdle && !M5.Mic.isRecording()) micIdle = true;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    micIdle = false;
    size_t n = rec_len;
    if (n + CHUNK > MAX_SAMPLES) { micOn = false; continue; }
    if (M5.Mic.record(&rec_buf[n], CHUNK, SAMPLE_RATE)) {
      rec_len = n + CHUNK;
      if (n >= CHUNK) {  // level of the previous (finished) chunk
        int32_t peak = 0;
        for (size_t i = n - CHUNK; i < n; ++i) peak = max<int32_t>(peak, abs(rec_buf[i]));
        level = min(40, (int)(peak / 400));
      }
    }
  }
}

static void startRecording() {
  M5.Mic.begin();
  rec_len = 0; level = 0;
  xSemaphoreTake(upMutex, portMAX_DELAY);
  upPartial = "";
  xSemaphoreGive(upMutex);
  upFinish = false; upDone = false; upStartFailed = false;
  micIdle = false; micOn = true;
  upStart = true;
  st = St::Recording;
}

static void finishPending() {
  if (replyOk) {
    turns.push_back({pending.heard, replyTarget});
    if (turns.size() > MAX_TURNS) turns.erase(turns.begin());
  } else {
    notice = replyTarget;
  }
  hasPending = false; pending = {};
  replyTarget = ""; replyShown = 0;
  st = St::Idle; textDirty = true; followTail = true;
  showHistory = true; historyUntil = millis() + HISTORY_SHOW_MS;
  M5.Power.setVibration(80); M5.delay(60); M5.Power.setVibration(0);
}

static void poll() {
  HTTPClient http;
  http.begin(String(RELAY_URL) + "/result?id=" + jobId);
  http.setTimeout(10000);
  addHeaders(http);
  int code = http.GET();
  if (code != 200) { http.end(); return; }  // transient: retry on next poll
  JsonDocument doc;
  deserializeJson(doc, http.getString());
  http.end();
  String status = doc["status"] | "error";
  if (status == "pending") {
    String p = doc["partial"] | "";
    if (p.length() > replyTarget.length()) replyTarget = p;
  } else {
    replyTarget = doc["reply"] | "不明なエラー";
    replyOk = status == "done";
    replyFinished = true;
  }
}

// BLE counterpart of poll(): the Mac pushes the reply, so this only takes what arrived.
static bool replyBle = false;   // the current reply arrives over BLE
static uint32_t bleSeenMs = 0;  // last time the link was up during this reply
static void pollBle(uint32_t now) {
  String payload;
  if (bleTake(BLE_REPLY_PARTIAL, payload)) {
    JsonDocument doc;
    if (!deserializeJson(doc, payload)) {
      String p = doc["partial"] | "";
      if (p.length() > replyTarget.length()) replyTarget = p;
    }
  }
  if (bleTake(BLE_REPLY_DONE, payload)) {
    JsonDocument doc;
    deserializeJson(doc, payload);
    String status = doc["status"] | "error";
    replyTarget = doc["reply"] | "不明なエラー";
    replyOk = status == "done";
    replyFinished = true;
    return;
  }
  if (bleReady()) bleSeenMs = now;
  else if (now - bleSeenMs > BLE_LOST_MS) {
    replyTarget = "Macとの接続が切れたよ";
    replyOk = false;
    replyFinished = true;
  }
}

// Reveal the reply a few characters at a time (UTF-8 aware).
static void typeReply(uint32_t now) {
  if (now - lastTypeMs < 100) return;
  size_t chars = (size_t)((now - lastTypeMs) * TYPE_CHARS_PER_SEC / 1000.0f);
  lastTypeMs = now;
  if (!replyOk && replyFinished) { replyShown = replyTarget.length(); }
  size_t before = replyShown;
  while (chars-- > 0 && replyShown < replyTarget.length()) {
    ++replyShown;
    while (replyShown < replyTarget.length() && (replyTarget[replyShown] & 0xC0) == 0x80) ++replyShown;
  }
  if (replyShown != before) textDirty = true;
  if (replyFinished && replyShown >= replyTarget.length()) finishPending();
}

static void resetSession() {
  bool ok = false;
  if (wifiUp()) {
    HTTPClient http;
    http.begin(String(RELAY_URL) + "/reset");
    http.setConnectTimeout(1500);
    addHeaders(http);
    ok = http.POST("") == 200;
    http.end();
  }
  String body;
  if (!ok && bleReady() && bleRequest(BLE_RESET, BLE_RESET_OK, body, 3000)) {
    JsonDocument doc;
    ok = !deserializeJson(doc, body) && (doc["ok"] | false);
  }
  if (ok) { turns.clear(); showNotice(""); }
  else showOffline("Macにつながらないよ", millis());
}

void switchToMenu();

static void backToMenu() { switchToMenu(); }

void switchToMenu() {
  const esp_partition_t* menu =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, nullptr);
  esp_app_desc_t desc;
  if (!menu || esp_ota_get_partition_description(menu, &desc) != ESP_OK) {
    showNotice("メニューが見つかりません");
    return;
  }
  M5.Display.fillScreen(TFT_BLACK);
  esp_ota_set_boot_partition(menu);
  M5.delay(300);
  esp_restart();
}

// Power key: M5Unified's BtnPWR waits for an M5PM1 interrupt flag that is not raised
// on the StopWatch, so read the key status register directly (as the stock menu does).
// Bit 0 is the current state, bit 7 a latched "was pressed" flag cleared by reading.
static bool powerKeyClicked(uint32_t now) {
  static constexpr uint8_t PM1_ADDR = 0x6E, PM1_REG_BTN_STATUS = 0x48;
  static uint32_t lastRead = 0, downAt = 0, fails = 0;
  static bool down = false;
  static uint8_t lastV = 0xFF;
  if (now - lastRead < 40) return false;
  lastRead = now;
  uint8_t v = 0;
  if (!M5.In_I2C.readRegister(PM1_ADDR, PM1_REG_BTN_STATUS, &v, 1, 100000)) {
    if ((++fails % 100) == 1) LOGF("power key: i2c read failed (%lu)\n", fails);
    return false;
  }
  bool first = lastV == 0xFF;
  if (v != lastV) { LOGF("power key: status 0x%02X\n", v); lastV = v; }
  if (first) { down = v & 0x01; return false; }  // a flag latched before this app started
  bool pressed = v & 0x01;
  bool latched = v & 0x80;
  if (pressed && !down) { down = true; downAt = now; return false; }
  if (!pressed && down) { down = false; LOGF("power key: held %lu ms\n", now - downAt); return now - downAt < 2000; }
  return latched && !pressed;  // pressed and released between two reads
}

// ---------- main ----------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(150);
  W = M5.Display.width(); H = M5.Display.height();
  CX = W / 2; CY = H / 2;
  TW = W * 0.74f; TXL = (W - TW) / 2;

  screen.setPsram(true); screen.setColorDepth(16); screen.createSprite(W, H);
  textCv.setPsram(true); textCv.setColorDepth(16); textCv.createSprite(TW, TEXT_CANVAS_H);
  // fonts are loaded once per canvas; drawing code never switches fonts
  screen.loadFont(FONT_NOTO);
  textCv.loadFont(FONT_NOTO);
  textCv.setTextWrap(true, false);
  fallbackCv.setColorDepth(16);
  fallbackCv.createSprite(28, 28);
  fallbackCv.setFont(&fonts::lgfxJapanGothic_24);
  fallbackCv.setTextDatum(textdatum_t::top_left);
  LOGF("glyph check: in noto: あ %d 丼 %d 嗅 %d A %d, fallback width 嗅 %d\n", inNoto("あ"), inNoto("丼"), inNoto("嗅"), inNoto("A"), fallbackCv.textWidth("嗅"));


  rec_buf = (int16_t*)heap_caps_malloc(MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  upMutex = xSemaphoreCreateMutex();

  M5.Speaker.end();
  auto mc = M5.Mic.config();
  mc.sample_rate = SAMPLE_RATE;
  mc.noise_filter_level = 64;
  M5.Mic.config(mc);
  // the mic is started only while the yellow key is held (see startRecording) to save power
  xTaskCreatePinnedToCore(micTask, "mic", 4096, nullptr, 5, nullptr, 1);
  xTaskCreatePinnedToCore(uploadTask, "upload", 8192, nullptr, 3, nullptr, 0);
  if (TRANSPORT_USE_BLE) bleBegin();  // link to the Mac when Wi-Fi / HTTP is not usable

  updateRotation();
  render();
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);  // modem sleep while idle; switched off during a conversation (loop)
  if (TRANSPORT_USE_WIFI) WiFi.begin(WIFI_SSID, WIFI_PASS);  // TRANSPORT=ble: the radio stays off for Wi-Fi
  LOGF("display %dx%d, psram free %u\n", W, H, ESP.getFreePsram());
}

void loop() {
  M5.update();
  uint32_t now = millis();
  if (st != St::Recording) updateRotation();

  // the history is loaded once when Wi-Fi first connects (quietly; if that fails,
  // it is tried again when the owner opens the history)
  static bool wifiSeen = false;
  if (!wifiSeen && WiFi.status() == WL_CONNECTED) {
    wifiSeen = true;
    LOGF("wifi %s\n", WiFi.localIP().toString().c_str());
    fetchHistory();
  }
  // without Wi-Fi, the history comes over BLE once the Mac has authenticated
  static bool bleWasReady = false;
  bool bleNow = bleReady();
  if (bleNow && !bleWasReady && !historyLoaded && !wifiUp() && st == St::Idle) fetchHistory();
  bleWasReady = bleNow;

  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) wakeEyes(now);
  updateBattery(now);
  if (powerKeyClicked(now)) {  // the power key has no job while the app runs: show the button tips
    LOGF("power key clicked\n");
    tipAUntil = tipBUntil = now + TIP_MS;
    wakeEyes(now);
  }
  bool busy = (st == St::Sending || st == St::Thinking);
  // yellow key with neither Wi-Fi nor BLE: a short tap still toggles the history,
  // holding shows "x" eyes
  static uint32_t offlinePressAt = 0;
  if (!busy && st != St::Recording && M5.BtnA.wasPressed()) {
    if (!wifiUp() && !bleReady()) offlinePressAt = now;
    else startRecording();
  }
  if (offlinePressAt) {
    if (M5.BtnA.isPressed()) {
      if (now - offlinePressAt > TAP_MS) showOffline("Wi-Fiにつながってないよ", now);
    } else {
      if (now - offlinePressAt <= TAP_MS) {
        showHistory = !showHistory; historyUntil = now + HISTORY_SHOW_MS; followTail = true;
      }
      offlinePressAt = 0;
    }
  }
  // relay unreachable while talking: "x" eyes for as long as the key is held
  if (st == St::Recording && upStartFailed && rec_len >= SAMPLE_RATE * MIN_SECONDS)
    showOffline("Macにつながらないよ", now);
  if (!busy && st != St::Recording) {
    if (M5.BtnB.wasHold()) backToMenu();          // held past M5Unified's hold threshold (500 ms)
    else if (M5.BtnB.wasClicked()) resetSession();  // short press
  }

  if (st == St::Recording && (!M5.BtnA.isPressed() || !micOn)) {
    micOn = false;
    while (!micIdle) M5.delay(1);  // every queued sample is filled now
    M5.Mic.end();
    upFinalLen = rec_len;
    upFinish = true;
    st = St::Sending;
  }

  if (st == St::Sending && upDone) {
    if (rec_len < SAMPLE_RATE * MIN_SECONDS) {
      st = St::Idle;  // a short tap on the yellow button: show / hide the history
      showHistory = !showHistory; historyUntil = now + HISTORY_SHOW_MS; followTail = true;
      if (showHistory && !historyLoaded) fetchHistory();
    } else if (upCode != 200) {
      showOffline("Macにつながらないよ", now);
      st = St::Idle;
    } else if (upJobId.isEmpty()) {
      st = St::Idle;  // nothing recognised: go back quietly
    } else {
      jobId = upJobId;
      pending = {upText, ""};
      hasPending = true; notice = "";
      replyTarget = ""; replyShown = 0; replyFinished = false; replyOk = true;
      lastPollMs = 0; lastTypeMs = now;
      replyBle = upBle; bleSeenMs = now;
      st = St::Thinking;
    }
    textDirty = true; followTail = true;
  }

  if (st == St::Thinking) {
    if (!replyFinished) {
      if (replyBle) pollBle(now);  // pushed by the Mac: no polling over BLE
      else if (now - lastPollMs > 150) { lastPollMs = now; poll(); }
    }
    typeReply(now);
  }

  // touch: on the eyes, they look at the finger; on the history, drag scrolls and a
  // tap goes back to the eyes (a short tap on the yellow button opens the history)
  auto t = M5.Touch.getDetail();
  bool historyVisible = st == St::Thinking ? replyShown > 0 : (st == St::Idle && showHistory);
  if (t.isPressed() && eyes.asleep) { wakeEyes(now); t = {}; }  // a touch only wakes her up
  if (t.isPressed()) wakeEyes(now);
  if (t.isPressed()) {
    if (historyVisible) {
      if (lastTouchY >= 0 && t.y != lastTouchY) { scrollY -= (t.y - lastTouchY); followTail = false; }
      historyUntil = now + HISTORY_SHOW_MS;
    } else if (st == St::Idle) {
      eyes.followingTouch = true;
      lookAt((t.x - CX) / (W / 2.0f), (t.y - CY) / (H / 2.0f));
    }
    lastTouchY = t.y;
  } else {
    if (eyes.followingTouch) { eyes.followingTouch = false; lookAt(0, 0); eyes.nextGlanceMs = now + 1500; }
    lastTouchY = -1;
  }
  if (st == St::Idle && !historyVisible) {
    detectShake(now);
    detectCircles(t, now);
    if (!eyes.asleep) detectPetting(t, now);
  }
  if (historyVisible && st == St::Idle && t.wasClicked()) showHistory = false;
  if (showHistory && st == St::Idle && now > historyUntil) showHistory = false;

  // power: Wi-Fi modem sleep and a low frame rate while idle, a dim screen while asleep
  static bool fastLink = false;
  bool talking = st != St::Idle;
  if (talking != fastLink) { fastLink = talking; WiFi.setSleep(!talking); }
  static int brightness = -1;
  int want = eyes.asleep ? 40 : 150;
  if (want != brightness) { brightness = want; M5.Display.setBrightness(want); }
  static uint32_t lastFrame = 0;
  uint32_t frameMs = talking ? 0 : (eyes.asleep ? 100 : 33);
  uint32_t spent = millis() - lastFrame;
  if (spent < frameMs) M5.delay(frameMs - spent);
  lastFrame = millis();
  render();
}
