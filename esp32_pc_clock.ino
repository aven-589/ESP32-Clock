/*
 * ESP32-S3 PC Clock
 * @版本 1.3.0
 * @日期 2026-11-09
 *
 * 独立时钟工程：不连接 WiFi，不使用 NTP。
 * 未同步前 ESP32 每秒广播 PC_CLOCK_HELLO，电脑脚本据此自动找到串口；
 * 第一次同步后停止广播并靠 ESP32 自身走时，电脑之后每小时校准一次。
 * 显示时、分、秒、月/日、星期；整屏约 25fps 刷新，右上角只保留 PC 图标，
 * 左下角加入一只会随机跑动、坐下、摇尾巴的小像素狗。
 * 支持掉电保存的闹钟：最多 8 条，单次/每天，自定义音量、时长和音符。
 *
 * 硬件：
 *   OLED SSD1306 128x64 I2C
 *   SDA -> GPIO1
 *   SCL -> GPIO2
 *   VCC -> 3.3V
 *   GND -> GND
 *   MAX98357A I2S 功放
 *   BCLK -> GPIO4, LRC/WS -> GPIO5, DIN -> GPIO7, SD -> GPIO8
 *   VIN -> 5V, GAIN -> GND（默认增益）
 *
 * 串口协议（115200 8N1）：
 *   PC -> ESP : SYNC <UTC秒> <时区偏移分钟>\n
 *   ESP -> PC : OK <UTC秒> <时区偏移分钟>\n
 *   ESP -> PC : PC_CLOCK_HELLO 1\n  （未首次同步前每秒广播）
 *   PC -> ESP : PING\n
 *   ESP -> PC : PONG PC_CLOCK 1\n
 *   PC -> ESP : ALARM LIST / ADD / DEL / CLEAR / ENABLE / TUNE / TEST / STOP\n
 *   ESP -> PC : OK ... 或 ERR ...\n
 */

#define U8X8_ESP32_NO_HW_I2C
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <Preferences.h>
#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define OLED_SDA 1
#define OLED_SCL 2
#define SERIAL_BAUD 115200
#define BEACON_INTERVAL_MS 1000
#define CLOCK_FRAME_MS 40

// MAX98357A / I2S speaker. These are the pins used by the earlier face
// project on the same board; change them here if your amplifier is wired
// differently.
#define SPK_BCLK 4
#define SPK_LRC 5
#define SPK_DIN 7
#define SPK_SD 8
#define SPK_SAMPLE_RATE 16000

#define ALARM_SLOTS 8
#define ALARM_TUNE_MAX 48
#define ALARM_MAGIC 0x414C524DUL  // "ALRM"

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

static time_t g_baseEpoch = 0;
static uint32_t g_baseMillis = 0;
static int g_tzOffsetMin = 0;
static uint32_t g_lastSyncMs = 0;
static uint32_t g_lastBeaconMs = 0;
static bool g_everSynced = false;
static bool g_pcOnline = false;

// Alarm storage. Eight records are only a few hundred bytes, so the whole
// table is kept in one NVS blob and survives power loss.
struct AlarmRecord {
  uint8_t used;
  uint8_t hour;
  uint8_t minute;
  uint8_t daily;
  uint8_t enabled;
  uint8_t sound;
  uint8_t volume;
  uint16_t duration;
  char tune[ALARM_TUNE_MAX];
};

static AlarmRecord g_alarms[ALARM_SLOTS];
static Preferences g_prefs;
static int32_t g_lastAlarmMinuteKey = -1;

static volatile bool g_alarmActive = false;
static volatile bool g_alarmStop = false;
static uint8_t g_alarmSound = 1;
static uint8_t g_alarmVolume = 70;
static uint16_t g_alarmDuration = 15;
static char g_alarmTune[ALARM_TUNE_MAX] = "";
static i2s_chan_handle_t g_spk_tx = nullptr;
static bool g_speakerReady = false;
static int16_t g_toneBuf[256 * 2];

struct ToneNote {
  uint16_t freq;
  uint16_t ms;
};

struct ClockDigits {
  enum { COUNT = 11, DURATION_MS = 360 };
  char previous[COUNT];
  char current[COUNT];
  uint32_t started;
  bool initialized;
  bool settlePending;

  ClockDigits() : started(0), initialized(false), settlePending(false) {
    memset(previous, '-', COUNT);
    memset(current, '-', COUNT);
  }

  void update(const char* next, uint32_t now) {
    if (!initialized) {
      memcpy(previous, next, COUNT);
      memcpy(current, next, COUNT);
      initialized = true;
      started = now;
      settlePending = false;
    } else if (memcmp(current, next, COUNT)) {
      memcpy(previous, current, COUNT);
      memcpy(current, next, COUNT);
      started = now;
      settlePending = true;
    }
  }

  // Only clear the animation after a frame at/after DURATION_MS has really
  // been drawn. Otherwise a last frame at 320ms may be deduplicated and a
  // digit can freeze halfway.
  void settle(uint32_t drawnAt) {
    if (settlePending && static_cast<uint32_t>(drawnAt - started) >= DURATION_MS) {
      settlePending = false;
    }
  }

  int offset(uint32_t now, int height) const {
    const uint32_t elapsed = now - started;
    if (elapsed >= DURATION_MS) return height;
    return static_cast<int>((static_cast<uint32_t>(height) * elapsed + DURATION_MS / 2) / DURATION_MS);
  }

  bool animating() const { return initialized && settlePending; }
};

static ClockDigits g_digits;

// The dog has its own little state machine, completely independent of the
// clock seconds. It picks a random destination, random speed and a random
// action (run / sit / wag) whenever the previous action finishes.
static constexpr float DOG_MIN_X = 4.0f;
static constexpr float DOG_MAX_X = 36.0f;

struct DogState {
  float x;
  float targetX;
  float speed;
  uint32_t lastTick;
  uint32_t actionUntil;
  uint32_t nextBlink;
  uint32_t blinkUntil;
  int8_t direction;  // +1 moving right, -1 moving left
  bool running;
  bool wagFast;
  bool blink;
};

static DogState g_dog = {18.0f, 18.0f, 20.0f, 0, 0, 0, 0, 1, false, true, false};

static void dogChooseAction(uint32_t now) {
  const int roll = random(100);
  if (roll < 70) {
    // Pick a destination away from the current position. Without this check a
    // random target equal to the current x could leave "running" stuck forever.
    float target = static_cast<float>(random(static_cast<int>(DOG_MIN_X), static_cast<int>(DOG_MAX_X) + 1));
    if (target > g_dog.x - 1.0f && target < g_dog.x + 1.0f) {
      target = (g_dog.x < (DOG_MIN_X + DOG_MAX_X) / 2.0f) ? DOG_MAX_X : DOG_MIN_X;
    }
    const float distance = target > g_dog.x ? target - g_dog.x : g_dog.x - target;
    g_dog.running = true;
    g_dog.targetX = target;
    g_dog.speed = static_cast<float>(random(12, 37));
    g_dog.actionUntil = now + static_cast<uint32_t>(distance / g_dog.speed * 1000.0f) + 400 + random(0, 500);
  } else if (roll < 85) {
    g_dog.running = false;
    g_dog.wagFast = false;
    g_dog.actionUntil = now + random(500, 1200);
  } else {
    g_dog.running = false;
    g_dog.wagFast = true;
    g_dog.actionUntil = now + random(600, 1400);
  }
}

static void dogTick(uint32_t now) {
  const uint32_t dtMs = now - g_dog.lastTick;
  g_dog.lastTick = now;

  if (!g_dog.running && now >= g_dog.actionUntil) {
    dogChooseAction(now);
  }
  if (g_dog.running) {
    if (g_dog.x == g_dog.targetX || now >= g_dog.actionUntil) {
      g_dog.running = false;
      g_dog.actionUntil = now + random(300, 900);
    } else {
      const float step = g_dog.speed * static_cast<float>(dtMs) / 1000.0f;
      if (g_dog.x < g_dog.targetX) {
        g_dog.direction = 1;
        g_dog.x += step;
        if (g_dog.x >= g_dog.targetX) {
          g_dog.x = g_dog.targetX;
          g_dog.running = false;
          g_dog.actionUntil = now + random(300, 900);
        }
      } else if (g_dog.x > g_dog.targetX) {
        g_dog.direction = -1;
        g_dog.x -= step;
        if (g_dog.x <= g_dog.targetX) {
          g_dog.x = g_dog.targetX;
          g_dog.running = false;
          g_dog.actionUntil = now + random(300, 900);
        }
      }
    }
  }

  if (g_dog.x < DOG_MIN_X) g_dog.x = DOG_MIN_X;
  if (g_dog.x > DOG_MAX_X) g_dog.x = DOG_MAX_X;

  if (now >= g_dog.nextBlink) {
    g_dog.blink = true;
    g_dog.blinkUntil = now + random(90, 220);
    g_dog.nextBlink = now + random(1800, 5000);
  }
  if (g_dog.blink && now >= g_dog.blinkUntil) g_dog.blink = false;
}

static bool localNow(struct tm& out) {
  if (!g_everSynced) return false;
  const uint32_t elapsed = millis() - g_baseMillis;
  const time_t localEpoch = g_baseEpoch + static_cast<time_t>(g_tzOffsetMin) * 60 + elapsed / 1000;
  gmtime_r(&localEpoch, &out);
  return true;
}

static void clockPair(char* out, int value) {
  out[0] = '0' + value / 10;
  out[1] = '0' + value % 10;
}

// Fixed aperture odometer: previous and next glyph move down together.
static void clockRoll(U8G2& s, const char* before, const char* after,
                      int x, int top, int width, int height,
                      int baseline, uint32_t now) {
  const int offset = g_digits.offset(now, height);
  s.setClipWindow(x, top, x + width, top + height);
  if (!strcmp(before, after) || offset >= height) {
    s.drawUTF8(x + (width - s.getUTF8Width(after)) / 2, baseline, after);
  } else {
    s.drawUTF8(x + (width - s.getUTF8Width(before)) / 2, baseline + offset, before);
    const int incomingY = baseline - height + offset;
    if (incomingY >= top) {
      s.drawUTF8(x + (width - s.getUTF8Width(after)) / 2, incomingY, after);
    }
  }
  s.setMaxClipWindow();
}

static void clockDigit(int index, int x, int top, int width, int height,
                       int baseline, uint32_t now) {
  const char before[] = {g_digits.previous[index], '\0'};
  const char after[] = {g_digits.current[index], '\0'};
  clockRoll(u8g2, before, after, x, top, width, height, baseline, now);
}

static void drawPcIcon(bool online) {
  // Small monitor in the same top-right slot as the old WiFi icon.
  u8g2.drawFrame(113, 1, 12, 8);
  u8g2.drawBox(117, 9, 4, 1);
  if (!online && ((millis() / 450) & 1)) {
    u8g2.drawLine(112, 1, 126, 11);
  }
}

// A small pixel dog in the bottom-left corner. It sleeps before the first
// sync (and late at night), wags its tail once per second, and blinks every
// seven seconds. Kept intentionally simple so it does not compete with time.
static void drawDog(bool awake, const struct tm* t) {
  (void)t;  // The dog no longer stops at night; it only sleeps before first sync.
  const uint32_t now = millis();
  const bool asleep = !awake;
  const bool running = g_dog.running && !asleep;
  const int x = static_cast<int>(g_dog.x + 0.5f);
  const int bob = running && ((now / 140) & 1) ? -1 : 0;
  const int y = 48 + bob;

  // Floppy ears.
  u8g2.drawLine(x + 1, y + 2, x + 1, y + 7);
  u8g2.drawLine(x + 2, y + 2, x + 2, y + 8);
  u8g2.drawLine(x + 14, y + 2, x + 14, y + 7);
  u8g2.drawLine(x + 15, y + 2, x + 15, y + 8);

  // Head, muzzle and body.
  u8g2.drawFrame(x + 3, y + 1, 10, 9);
  u8g2.drawFrame(x + 5, y + 9, 7, 6);

  // Legs alternate only while running; sitting keeps both feet planted.
  if (running) {
    const int leg = ((now / 120) & 1) ? 1 : 0;
    u8g2.drawLine(x + 5, y + 13 + leg, x + 5, y + 15);
    u8g2.drawLine(x + 11, y + 13 + (1 - leg), x + 11, y + 15);
  } else {
    u8g2.drawLine(x + 5, y + 14, x + 5, y + 15);
    u8g2.drawLine(x + 11, y + 14, x + 11, y + 15);
  }

  // Tail speed is part of the action, not the clock second. The tail trails
  // behind the dog: moving right puts it on the left side and vice versa.
  const uint32_t tailPeriod = (g_dog.wagFast || running) ? 120 : 240;
  const int wag = ((now / tailPeriod) & 1) ? -1 : 1;
  const int tailBaseX = (g_dog.direction > 0) ? x + 4 : x + 12;
  const int tailElbowX = (g_dog.direction > 0) ? x + 1 : x + 16;
  const int tailTipX = (g_dog.direction > 0) ? x - 1 : x + 17;
  u8g2.drawLine(tailBaseX, y + 13, tailElbowX, y + 11 + wag * 2);
  u8g2.drawLine(tailElbowX, y + 11 + wag * 2, tailTipX, y + 8 + wag * 2);

  // Eyes blink on their own random schedule.
  if (asleep || g_dog.blink) {
    u8g2.drawLine(x + 5, y + 5, x + 7, y + 5);
    u8g2.drawLine(x + 9, y + 5, x + 11, y + 5);
  } else {
    u8g2.drawBox(x + 5, y + 4, 2, 2);
    u8g2.drawBox(x + 9, y + 4, 2, 2);
  }
  u8g2.drawBox(x + 7, y + 8, 2, 1);
  u8g2.drawPixel(x + 7, y + 10);

  if (asleep) {
    const uint8_t phase = (now / 500) % 3;
    u8g2.setFont(u8g2_font_5x7_tn);
    u8g2.drawStr(x + 16, y + 1 - phase, "z");
  }
}

static void drawNoTime() {
  u8g2.setFont(u8g2_font_logisoso32_tn);
  const char* dashes = "--:--";
  const int width = u8g2.getUTF8Width(dashes);
  u8g2.drawUTF8((128 - width) / 2, 43, dashes);

  u8g2.setFont(u8g2_font_logisoso16_tn);
  u8g2.drawStr(104, 43, "--");

  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  u8g2.drawUTF8(62, 62, "--/--");
  u8g2.drawUTF8(100, 62, "--");

  drawDog(false, nullptr);
  drawPcIcon(false);
}

static void sendClockFrame() {
  // Requested behaviour: send a complete frame once per second. This is much
  // more robust than partial tile updates and only costs about 31ms of I2C
  // per second.
  u8g2.sendBuffer();
}

static void drawClockFace() {
  struct tm time;
  const bool valid = localNow(time);
  const uint32_t now = millis();

  u8g2.clearBuffer();
  u8g2.setDrawColor(1);

  if (!valid) {
    drawNoTime();
    sendClockFrame();
    return;
  }

  char next[ClockDigits::COUNT];
  clockPair(next, time.tm_hour);
  clockPair(next + 2, time.tm_min);
  clockPair(next + 4, time.tm_sec);
  clockPair(next + 6, time.tm_mon + 1);
  clockPair(next + 8, time.tm_mday);
  next[10] = '0' + time.tm_wday;
  g_digits.update(next, now);

  u8g2.setFont(u8g2_font_logisoso32_tn);
  const int digitX[] = {1, 23, 53, 75};
  for (int i = 0; i < 4; ++i) {
    clockDigit(i, digitX[i], 10, 22, 36, 43, now);
  }

  // Restored half-second colon blink.
  if ((millis() / 500) & 1) {
    u8g2.drawBox(47, 22, 3, 3);
    u8g2.drawBox(47, 34, 3, 3);
  }

  u8g2.setFont(u8g2_font_logisoso16_tn);
  clockDigit(4, 104, 26, 10, 19, 43, now);
  clockDigit(5, 115, 26, 10, 19, 43, now);

  // Date and weekday now use the same 12px Chinese font. The date is
  // right-aligned eight pixels to the left of the weekday.
  u8g2.setFont(u8g2_font_wqy12_t_gb2312);
  char beforeDate[6] = {
      g_digits.previous[6], g_digits.previous[7], '/',
      g_digits.previous[8], g_digits.previous[9], '\0'};
  char afterDate[6] = {
      g_digits.current[6], g_digits.current[7], '/',
      g_digits.current[8], g_digits.current[9], '\0'};
  const int dateWidth = u8g2.getUTF8Width(afterDate);
  const int dateX = 100 - 8 - dateWidth;
  clockRoll(u8g2, beforeDate, afterDate, dateX, 52, dateWidth, 12, 62, now);

  const char* weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  const char oldDay = g_digits.previous[10];
  const char newDay = g_digits.current[10];
  clockRoll(u8g2,
            oldDay == '-' ? "对时" : weekdays[oldDay - '0'],
            newDay == '-' ? "对时" : weekdays[newDay - '0'],
            100, 52, 25, 12, 62, now);

  drawDog(true, &time);
  drawPcIcon(g_pcOnline);
  g_digits.settle(now);
  sendClockFrame();
}

// ---------------------------------------------------------------------------
// Alarm storage, speaker and serial command handling
// ---------------------------------------------------------------------------

static void alarmDefault(AlarmRecord& a) {
  memset(&a, 0, sizeof(a));
  a.sound = 1;      // chime
  a.volume = 70;
  a.duration = 15;
}

static void loadAlarms() {
  for (uint8_t i = 0; i < ALARM_SLOTS; ++i) alarmDefault(g_alarms[i]);
  g_prefs.begin("clockalarm", false);
  if (g_prefs.getUInt("magic", 0) != ALARM_MAGIC) return;
  if (g_prefs.getBytesLength("list") != sizeof(g_alarms)) return;
  g_prefs.getBytes("list", g_alarms, sizeof(g_alarms));
  for (uint8_t i = 0; i < ALARM_SLOTS; ++i) {
    AlarmRecord& a = g_alarms[i];
    if (a.hour > 23 || a.minute > 59 || a.sound > 4 ||
        a.volume > 100 || a.duration == 0 || a.duration > 300) {
      alarmDefault(a);
    }
    a.tune[ALARM_TUNE_MAX - 1] = '\0';
  }
}

static void saveAlarms() {
  g_prefs.putUInt("magic", ALARM_MAGIC);
  g_prefs.putBytes("list", g_alarms, sizeof(g_alarms));
}

static const char* alarmSoundName(uint8_t sound) {
  switch (sound) {
    case 0: return "beep";
    case 1: return "chime";
    case 2: return "bird";
    case 3: return "alarm";
    case 4: return "custom";
    default: return "chime";
  }
}

static bool parseAlarmSound(const char* text, uint8_t& sound) {
  if (!strcmp(text, "beep")) { sound = 0; return true; }
  if (!strcmp(text, "chime")) { sound = 1; return true; }
  if (!strcmp(text, "bird")) { sound = 2; return true; }
  if (!strcmp(text, "alarm")) { sound = 3; return true; }
  if (!strcmp(text, "custom")) { sound = 4; return true; }
  return false;
}

static bool parseAlarmTime(const char* text, uint8_t& hour, uint8_t& minute) {
  int h = 0, m = 0;
  char tail = 0;
  if (sscanf(text, "%d:%d%c", &h, &m, &tail) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  hour = static_cast<uint8_t>(h);
  minute = static_cast<uint8_t>(m);
  return true;
}

static uint16_t noteFrequency(char letter, int accidental, int octave) {
  int semitone;
  switch (letter) {
    case 'C': case 'c': semitone = 0; break;
    case 'D': case 'd': semitone = 2; break;
    case 'E': case 'e': semitone = 4; break;
    case 'F': case 'f': semitone = 5; break;
    case 'G': case 'g': semitone = 7; break;
    case 'A': case 'a': semitone = 9; break;
    case 'B': case 'b': semitone = 11; break;
    default: return 0;
  }
  const int midi = 12 * (octave + 1) + semitone + accidental;
  const float hz = 440.0f * powf(2.0f, static_cast<float>(midi - 69) / 12.0f);
  if (hz < 60.0f || hz > 5000.0f) return 0;
  return static_cast<uint16_t>(hz + 0.5f);
}

// Pattern syntax: C5:180,E5:180,G5:360,0:120
// Each item is <note>:<milliseconds>; 0 is a rest. Max ALARM_TUNE_MAX chars.
static uint8_t parseTune(const char* text, ToneNote* notes, uint8_t maxNotes) {
  if (!text || !*text) return 0;
  char copy[ALARM_TUNE_MAX];
  strncpy(copy, text, sizeof(copy) - 1);
  copy[sizeof(copy) - 1] = '\0';

  uint8_t count = 0;
  char* save = nullptr;
  char* token = strtok_r(copy, ",", &save);
  while (token && count < maxNotes) {
    char* colon = strchr(token, ':');
    if (colon) {
      *colon = '\0';
      int ms = atoi(colon + 1);
      if (ms < 20) ms = 20;
      if (ms > 3000) ms = 3000;
      const char* p = token;
      while (*p == ' ' || *p == '\t') ++p;
      uint16_t freq = 0;
      if (*p != '0' && *p != '\0') {
        const char letter = *p++;
        int accidental = 0;
        if (*p == '#') { accidental = 1; ++p; }
        else if (*p == 'b' || *p == 'B') { accidental = -1; ++p; }
        int octave = 5;
        if (*p >= '0' && *p <= '9') octave = *p++ - '0';
        freq = noteFrequency(letter, accidental, octave);
      }
      notes[count].freq = freq;
      notes[count].ms = static_cast<uint16_t>(ms);
      ++count;
    }
    token = strtok_r(nullptr, ",", &save);
  }
  return count;
}

static uint8_t presetTune(uint8_t sound, ToneNote* notes, uint8_t maxNotes) {
  static const ToneNote beep[] = {{880, 180}, {0, 120}, {880, 180}, {0, 600}};
  static const ToneNote chime[] = {{523, 180}, {659, 180}, {784, 180}, {1047, 360}, {0, 600}};
  static const ToneNote bird[] = {{1200, 80}, {1500, 60}, {1800, 80}, {0, 80}, {1400, 80}, {0, 600}};
  static const ToneNote alarm[] = {{660, 180}, {880, 180}, {660, 180}, {880, 180}, {0, 400}};
  const ToneNote* source = chime;
  uint8_t count = sizeof(chime) / sizeof(chime[0]);
  if (sound == 0) { source = beep; count = sizeof(beep) / sizeof(beep[0]); }
  else if (sound == 2) { source = bird; count = sizeof(bird) / sizeof(bird[0]); }
  else if (sound == 3) { source = alarm; count = sizeof(alarm) / sizeof(alarm[0]); }
  else if (sound == 4) {
    count = parseTune(g_alarmTune, notes, maxNotes);
    if (count) return count;
    source = chime;
    count = sizeof(chime) / sizeof(chime[0]);
  }
  if (count > maxNotes) count = maxNotes;
  memcpy(notes, source, count * sizeof(ToneNote));
  return count;
}

static bool speakerInit() {
  if (SPK_SD >= 0) {
    pinMode(SPK_SD, OUTPUT);
    digitalWrite(SPK_SD, HIGH);
  }

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  if (i2s_new_channel(&chan_cfg, &g_spk_tx, nullptr) != ESP_OK) return false;

  i2s_std_config_t std_cfg = {};
  std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPK_SAMPLE_RATE);
  std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.bclk = static_cast<gpio_num_t>(SPK_BCLK);
  std_cfg.gpio_cfg.ws = static_cast<gpio_num_t>(SPK_LRC);
  std_cfg.gpio_cfg.dout = static_cast<gpio_num_t>(SPK_DIN);
  std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
  std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
  std_cfg.gpio_cfg.invert_flags.ws_inv = false;

  if (i2s_channel_init_std_mode(g_spk_tx, &std_cfg) != ESP_OK) {
    i2s_del_channel(g_spk_tx);
    g_spk_tx = nullptr;
    return false;
  }
  if (i2s_channel_enable(g_spk_tx) != ESP_OK) {
    i2s_del_channel(g_spk_tx);
    g_spk_tx = nullptr;
    return false;
  }
  return true;
}

static void alarmTask(void*) {
  g_speakerReady = speakerInit();
  if (!g_speakerReady) Serial.println("ERR speaker init");

  while (true) {
    if (!g_alarmActive) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!g_speakerReady) {
      g_alarmActive = false;
      continue;
    }

    ToneNote notes[12];
    uint8_t count = presetTune(g_alarmSound, notes, 12);
    if (!count) {
      g_alarmActive = false;
      continue;
    }
    const uint8_t volume = g_alarmVolume;
    const uint32_t end = millis() + static_cast<uint32_t>(g_alarmDuration) * 1000UL;
    const int32_t amplitude = static_cast<int32_t>(12000) * volume / 100;
    uint8_t index = 0;
    uint32_t noteStarted = millis();
    float phase = 0.0f;

    while (g_alarmActive && !g_alarmStop && millis() < end) {
      const ToneNote note = notes[index];
      if (millis() - noteStarted >= note.ms) {
        ++index;
        if (index >= count) index = 0;
        noteStarted = millis();
        phase = 0.0f;
        continue;
      }

      for (int i = 0; i < 256; ++i) {
        int16_t sample = 0;
        if (note.freq && amplitude > 0) {
          phase += 6.283185307f * static_cast<float>(note.freq) / SPK_SAMPLE_RATE;
          if (phase >= 6.283185307f) phase -= 6.283185307f;
          sample = static_cast<int16_t>(sinf(phase) * amplitude);
        }
        g_toneBuf[i * 2] = sample;
        g_toneBuf[i * 2 + 1] = sample;
      }
      size_t written = 0;
      if (i2s_channel_write(g_spk_tx, g_toneBuf, sizeof(g_toneBuf), &written, portMAX_DELAY) != ESP_OK) break;
    }
    g_alarmActive = false;
  }
}

static void startAlarmValues(uint8_t sound, uint8_t volume, uint16_t duration, const char* tune) {
  if (g_alarmActive) return;
  g_alarmSound = sound;
  g_alarmVolume = volume;
  g_alarmDuration = duration;
  strncpy(g_alarmTune, tune ? tune : "", sizeof(g_alarmTune) - 1);
  g_alarmTune[sizeof(g_alarmTune) - 1] = '\0';
  g_alarmStop = false;
  g_alarmActive = true;
  Serial.printf("ALARM RING %s %u%% %us\n", alarmSoundName(sound), volume, duration);
}

static void startAlarmSlot(uint8_t slot) {
  const AlarmRecord& a = g_alarms[slot];
  startAlarmValues(a.sound, a.volume, a.duration, a.tune);
}

static void alarmTick() {
  if (!g_everSynced || g_alarmActive) return;
  struct tm t;
  if (!localNow(t)) return;
  const int32_t key = static_cast<int32_t>(t.tm_year + 1900) * 10000L +
                      static_cast<int32_t>(t.tm_yday + 1) * 100L +
                      t.tm_hour * 60 + t.tm_min;
  if (key == g_lastAlarmMinuteKey) return;
  g_lastAlarmMinuteKey = key;

  for (uint8_t i = 0; i < ALARM_SLOTS; ++i) {
    AlarmRecord& a = g_alarms[i];
    if (!a.used || !a.enabled) continue;
    if (a.hour != t.tm_hour || a.minute != t.tm_min) continue;
    startAlarmSlot(i);
    if (!a.daily) {
      a.used = 0;
      saveAlarms();
    }
    break;
  }
}

static bool parseSlot(const char* text, uint8_t& slot) {
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (end == text || *end != '\0' || value < 0 || value >= ALARM_SLOTS) return false;
  slot = static_cast<uint8_t>(value);
  return true;
}

static void handleAlarmCommand(char* args) {
  char* save = nullptr;
  char* sub = strtok_r(args, " \t", &save);
  if (!sub) {
    Serial.println("ERR ALARM missing");
    return;
  }

  if (!strcmp(sub, "LIST")) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < ALARM_SLOTS; ++i) {
      const AlarmRecord& a = g_alarms[i];
      if (!a.used) continue;
      Serial.printf("ALARM %u %02u:%02u %s %s %s %u %u\n",
                    i, a.hour, a.minute, a.daily ? "daily" : "once",
                    a.enabled ? "on" : "off", alarmSoundName(a.sound),
                    a.volume, a.duration);
      ++count;
    }
    Serial.printf("OK ALARM LIST %u\n", count);
    return;
  }

  if (!strcmp(sub, "CLEAR")) {
    for (uint8_t i = 0; i < ALARM_SLOTS; ++i) alarmDefault(g_alarms[i]);
    saveAlarms();
    Serial.println("OK ALARM CLEAR");
    return;
  }

  if (!strcmp(sub, "STOP")) {
    g_alarmStop = true;
    Serial.println("OK ALARM STOP");
    return;
  }

  if (!strcmp(sub, "ADD")) {
    char* timeText = strtok_r(nullptr, " \t", &save);
    char* modeText = strtok_r(nullptr, " \t", &save);
    uint8_t hour, minute;
    if (!timeText || !modeText || !parseAlarmTime(timeText, hour, minute)) {
      Serial.println("ERR ALARM time");
      return;
    }
    const bool daily = !strcmp(modeText, "daily");
    if (!daily && strcmp(modeText, "once")) {
      Serial.println("ERR ALARM mode");
      return;
    }

    uint8_t sound = 1;
    uint8_t volume = 70;
    uint16_t duration = 15;
    char* soundText = strtok_r(nullptr, " \t", &save);
    if (soundText && !parseAlarmSound(soundText, sound)) {
      Serial.println("ERR ALARM sound");
      return;
    }
    char* volumeText = strtok_r(nullptr, " \t", &save);
    if (volumeText) {
      const int v = atoi(volumeText);
      if (v < 0 || v > 100) { Serial.println("ERR ALARM volume"); return; }
      volume = static_cast<uint8_t>(v);
    }
    char* durationText = strtok_r(nullptr, " \t", &save);
    if (durationText) {
      const int d = atoi(durationText);
      if (d < 1 || d > 300) { Serial.println("ERR ALARM duration"); return; }
      duration = static_cast<uint16_t>(d);
    }

    int slot = -1;
    for (uint8_t i = 0; i < ALARM_SLOTS; ++i) {
      if (!g_alarms[i].used) { slot = i; break; }
    }
    if (slot < 0) {
      Serial.println("ERR ALARM full");
      return;
    }
    AlarmRecord& a = g_alarms[slot];
    alarmDefault(a);
    a.used = 1;
    a.hour = hour;
    a.minute = minute;
    a.daily = daily ? 1 : 0;
    a.enabled = 1;
    a.sound = sound;
    a.volume = volume;
    a.duration = duration;
    saveAlarms();
    Serial.printf("OK ALARM ADD %d\n", slot);
    return;
  }

  if (!strcmp(sub, "DEL")) {
    char* slotText = strtok_r(nullptr, " \t", &save);
    uint8_t slot;
    if (!slotText || !parseSlot(slotText, slot) || !g_alarms[slot].used) {
      Serial.println("ERR ALARM slot");
      return;
    }
    alarmDefault(g_alarms[slot]);
    saveAlarms();
    Serial.println("OK ALARM DEL");
    return;
  }

  if (!strcmp(sub, "ENABLE")) {
    char* slotText = strtok_r(nullptr, " \t", &save);
    char* valueText = strtok_r(nullptr, " \t", &save);
    uint8_t slot;
    if (!slotText || !valueText || !parseSlot(slotText, slot) || !g_alarms[slot].used) {
      Serial.println("ERR ALARM slot");
      return;
    }
    const int value = atoi(valueText);
    if (value != 0 && value != 1) {
      Serial.println("ERR ALARM value");
      return;
    }
    g_alarms[slot].enabled = value;
    saveAlarms();
    Serial.printf("OK ALARM ENABLE %u %d\n", slot, value);
    return;
  }

  if (!strcmp(sub, "TUNE")) {
    char* slotText = strtok_r(nullptr, " \t", &save);
    char* pattern = strtok_r(nullptr, " \t", &save);
    uint8_t slot;
    if (!slotText || !pattern || !parseSlot(slotText, slot) || !g_alarms[slot].used) {
      Serial.println("ERR ALARM slot");
      return;
    }
    if (strlen(pattern) >= ALARM_TUNE_MAX) {
      Serial.println("ERR ALARM tune");
      return;
    }
    strncpy(g_alarms[slot].tune, pattern, ALARM_TUNE_MAX - 1);
    g_alarms[slot].tune[ALARM_TUNE_MAX - 1] = '\0';
    g_alarms[slot].sound = 4;
    saveAlarms();
    Serial.printf("OK ALARM TUNE %u\n", slot);
    return;
  }

  if (!strcmp(sub, "TEST")) {
    uint8_t sound = 1;
    uint8_t volume = 70;
    uint16_t duration = 5;
    char* first = strtok_r(nullptr, " \t", &save);
    uint8_t slot;
    // ALARM TEST <slot> tests the exact saved alarm, including custom tune.
    if (first && parseSlot(first, slot)) {
      if (!g_alarms[slot].used) {
        Serial.println("ERR ALARM slot");
        return;
      }
      startAlarmSlot(slot);
      Serial.printf("OK ALARM TEST %u\n", slot);
      return;
    }
    char* soundText = first;
    if (soundText && !parseAlarmSound(soundText, sound)) {
      Serial.println("ERR ALARM sound");
      return;
    }
    char* volumeText = strtok_r(nullptr, " \t", &save);
    if (volumeText) {
      const int v = atoi(volumeText);
      if (v < 0 || v > 100) { Serial.println("ERR ALARM volume"); return; }
      volume = static_cast<uint8_t>(v);
    }
    char* durationText = strtok_r(nullptr, " \t", &save);
    if (durationText) {
      const int d = atoi(durationText);
      if (d < 1 || d > 300) { Serial.println("ERR ALARM duration"); return; }
      duration = static_cast<uint16_t>(d);
    }
    startAlarmValues(sound, volume, duration, g_alarmTune);
    Serial.println("OK ALARM TEST");
    return;
  }

  Serial.println("ERR ALARM unknown");
}

static void processLine(char* line) {
  if (line[0] == '\0') return;

  if (!strncmp(line, "SYNC", 4) && (line[4] == ' ' || line[4] == '\t')) {
    char* p = line + 4;
    while (*p == ' ' || *p == '\t') ++p;
    char* end = nullptr;
    const long long epoch = strtoll(p, &end, 10);
    if (end == p) {
      Serial.println("ERR epoch");
      return;
    }
    while (*end == ' ' || *end == '\t') ++end;
    const long offset = strtol(end, &end, 10);
    while (*end == ' ' || *end == '\t') ++end;
    if (*end != '\0') {
      Serial.println("ERR format");
      return;
    }
    if (epoch < 1600000000LL || offset < -840 || offset > 840) {
      Serial.println("ERR range");
      return;
    }

    g_baseEpoch = static_cast<time_t>(epoch);
    g_tzOffsetMin = static_cast<int>(offset);
    g_baseMillis = millis();
    g_lastSyncMs = g_baseMillis;
    g_everSynced = true;
    g_pcOnline = true;
    Serial.printf("OK %lld %ld\n", epoch, offset);
    return;
  }

  if (!strcmp(line, "PING")) {
    Serial.println("PONG PC_CLOCK 1");
    return;
  }
  if (!strcmp(line, "HELLO")) {
    Serial.println("PC_CLOCK_READY 1");
    return;
  }
  if (!strncmp(line, "ALARM", 5) && (line[5] == ' ' || line[5] == '\t' || line[5] == '\0')) {
    handleAlarmCommand(line + 5);
    return;
  }
}

static void handleSerial() {
  static char line[160];
  static size_t length = 0;
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\r') continue;
    if (c == '\n') {
      line[length] = '\0';
      if (length) processLine(line);
      length = 0;
    } else if (length < sizeof(line) - 1) {
      line[length++] = static_cast<char>(c);
    } else {
      length = 0;  // Drop an overlong line rather than overflow.
    }
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(150);

  Wire.begin(OLED_SDA, OLED_SCL, 400000);
  u8g2.begin();
  Wire.setClock(400000);
  u8g2.setContrast(255);

  randomSeed(micros());
  g_dog.lastTick = millis();
  g_dog.actionUntil = millis() + 600;
  g_dog.nextBlink = millis() + random(1500, 4000);

  loadAlarms();
  Serial.println("PC_CLOCK_READY 1");
  g_lastBeaconMs = millis();

  xTaskCreatePinnedToCore(alarmTask, "alarm", 6144, nullptr, 2, nullptr, 0);
}

void loop() {
  handleSerial();

  // Until the first valid SYNC, advertise ourselves on every serial port.
  // The PC script listens for this beacon before falling back to PING.
  // Once synced, stop beaconing and run from the ESP32 clock.
  if (!g_everSynced && millis() - g_lastBeaconMs >= BEACON_INTERVAL_MS) {
    g_lastBeaconMs = millis();
    Serial.println("PC_CLOCK_HELLO 1");
  }

  // A successful first sync means the PC provided the time; after that the
  // ESP32 runs autonomously, so do not show "waiting" between hourly syncs.
  g_pcOnline = g_everSynced;
  alarmTick();

  // Full 128x64 frames at ~25 fps: the dog and the odometer need more than
  // one frame per second to move smoothly. Each frame is compact enough for
  // 400 kHz I2C, and this also avoids partial-update artefacts.
  static uint32_t lastFrame = 0;
  const uint32_t now = millis();
  if (now - lastFrame >= CLOCK_FRAME_MS) {
    lastFrame = now;
    dogTick(now);
    drawClockFace();
  }
  delay(5);
}
