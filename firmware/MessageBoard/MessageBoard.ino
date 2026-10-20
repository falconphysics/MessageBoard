/*
 * PhotoPainter Message Board
 * ESP32-S3-PhotoPainter (7.3" Spectra-6, 800x480) as a remote family message board.
 *
 * Each wake cycle:
 *   1. Bring up Wi-Fi and sync the clock
 *   2. Fetch the current messages from the Apps Script web app
 *   3. Compare the content hash against what's already on the panel
 *   4. Redraw ONLY if something changed (the refresh takes ~25s and flashes)
 *   5. Deep sleep until the next check
 *
 * BOOT button forces an immediate check-and-refresh.
 *
 * Libraries (Library Manager):
 *   GxEPD2          by Jean-Marc Zingg
 *   Adafruit GFX    by Adafruit
 *   ArduinoJson     by Benoit Blanchon  (v7)
 *
 * Board: "ESP32S3 Dev Module" with PSRAM enabled (OPI), USB CDC On Boot enabled.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <time.h>
#include <esp_sleep.h>
#include <driver/gpio.h>

#include <GxEPD2_7C.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSans18pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>

#include "board_pins.h"
#include "config.h"
#include "pmu.h"
#include "panel.h"

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

// Page height of HEIGHT/4 = 120 rows -> a 48 KB pixel buffer, drawn in 4 passes.
// Holding the whole 800x480 frame at 4bpp would be 192 KB of static RAM.
// PhotoPainterPanel is GxEPD2's GDEP073E01 with a longer busy timeout — see panel.h.
GxEPD2_7C<PhotoPainterPanel, PhotoPainterPanel::HEIGHT / 4> display(
    PhotoPainterPanel(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

// ---------------------------------------------------------------------------
// State that survives deep sleep
// ---------------------------------------------------------------------------

RTC_DATA_ATTR char rtcLastHash[40] = "";
RTC_DATA_ATTR int  rtcLastRenderedYday = -1;
RTC_DATA_ATTR uint32_t rtcFailureCount = 0;
RTC_DATA_ATTR bool rtcShowingOfflineNotice = false;
RTC_DATA_ATTR uint32_t rtcBootCount = 0;
RTC_DATA_ATTR time_t rtcLastRefreshEpoch = 0;

// Battery, read once per wake and appended to the footer.
String gBatteryNote;
bool   gBatteryLow = false;

// Whether the panel's power rails came up this wake. If the power chip didn't
// answer, a "render" goes to an unpowered panel and silently does nothing, so
// the board must not record the new content as shown.
bool   gPanelPowered = false;

// Messages that didn't fit on screen this render; shown as "+N more" in the
// footer. Set by drawMessages(), read by drawFooter().
int    gHiddenCount = 0;

// After this many consecutive failures (~12h at 20 min), tell them the board
// has gone stale rather than silently showing week-old messages.
static const uint32_t OFFLINE_NOTICE_AFTER = (12 * 60) / CHECK_INTERVAL_MINUTES;

// ---------------------------------------------------------------------------
// Message model
// ---------------------------------------------------------------------------

struct Message {
  String from;
  String text;
  bool   pinned;
};

// Room to receive up to 8 from the Apps Script (its MAX_MESSAGES decides how
// many it sends). About 4 fit on screen; any that don't are counted in the
// footer as "+N more" rather than dropped silently.
static const int MAX_MESSAGES = 8;
Message   messages[MAX_MESSAGES];
int       messageCount = 0;
String    contentHash;

// ---------------------------------------------------------------------------
// Layout constants (800 x 480)
// ---------------------------------------------------------------------------

static const int MARGIN        = 28;
static const int HEADER_H      = 72;
static const int FOOTER_H      = 34;
static const int CONTENT_TOP   = HEADER_H + 16;
static const int CONTENT_BOTTOM= EPD_HEIGHT - FOOTER_H - 10;
static const int CONTENT_W     = EPD_WIDTH - (2 * MARGIN);

// ---------------------------------------------------------------------------
// Forward declarations. The Arduino preprocessor auto-generates prototypes,
// but it does so unreliably for functions returning String or taking
// `struct tm*`, so declare them explicitly.
// ---------------------------------------------------------------------------

void     holdPmuRail();
void     blinkGreen(int times, int ms);
void     readBattery();
const char* wakeCauseName(esp_sleep_wakeup_cause_t cause);
void     sleepUntilNextCheck();
void     handleFailure();
bool     connectWiFi();
void     syncClock();
bool     fetchMessages();
String   toDisplayable(const String& in);
String   urlEncode(const char* s);
int16_t  textWidth(const String& s);
int      wrapText(const String& text, int maxWidth, int maxLines, String* lines);
uint16_t accentFor(int index, bool pinned);
void     renderBoard(struct tm* now, bool offlineNotice);
void     drawHeader(struct tm* now);
void     drawMessages();
void     drawEmptyState();
void     drawOfflineNotice();
void     drawFooter(struct tm* now, bool offlineNotice);

// ===========================================================================
// Setup — the whole program runs once, then sleeps.
// ===========================================================================

void setup() {
  Serial.begin(115200);
  delay(200);

  holdPmuRail();

  // The panel is powered from the PMU's ALDO3/ALDO4 rails, which the stock
  // firmware leaves switched off. Without this the SPI writes go nowhere and
  // GxEPD2 reports a refresh measured in microseconds.
  gPanelPowered = pmuInitPanelRails();

  pinMode(PIN_LED_GREEN, OUTPUT);
  digitalWrite(PIN_LED_GREEN, LOW);

  rtcBootCount++;

  // Three quick blinks at the top of every wake. This is the only evidence of
  // a wake you can see without Serial Monitor attached — if the board is
  // cycling correctly you'll catch these every CHECK_INTERVAL_MINUTES.
  blinkGreen(3, 120);

  const bool forcedByButton = (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1);
  Serial.printf("\n=== wake #%lu (%s) ===\n",
                (unsigned long)rtcBootCount,
                wakeCauseName(esp_sleep_get_wakeup_cause()));

  readBattery();

  if (!connectWiFi()) {
    Serial.println("wifi failed");
    handleFailure();
    sleepUntilNextCheck();
  }

  syncClock();

  struct tm now;
  const bool haveTime = getLocalTime(&now, 5000);

  if (!fetchMessages()) {
    Serial.println("fetch failed");
    handleFailure();
    sleepUntilNextCheck();
  }

  rtcFailureCount = 0;
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);

  // Redraw only when there's a reason to.
  const bool contentChanged = (contentHash != String(rtcLastHash));
  const bool dayChanged     = DAILY_FORCED_REFRESH && haveTime &&
                              (now.tm_yday != rtcLastRenderedYday);
  const bool clearingNotice = rtcShowingOfflineNotice;

  if (!contentChanged && !dayChanged && !clearingNotice && !forcedByButton
      && !DEBUG_FAST_CYCLE) {
    Serial.println("nothing changed - skipping refresh");
    sleepUntilNextCheck();
  }

  // Rate limit. The refresh flashes hard, so batch changes that arrive close
  // together rather than flashing once per message. A change after a quiet
  // spell is unaffected — this only holds back a second refresh that would
  // land soon after the first. The BOOT button always overrides.
  if (haveTime && rtcLastRefreshEpoch != 0 && !forcedByButton && !DEBUG_FAST_CYCLE) {
    const time_t nowEpoch = mktime(&now);
    const long sinceLast = (long)(nowEpoch - rtcLastRefreshEpoch);
    const long minGap = (long)MIN_MINUTES_BETWEEN_REFRESHES * 60L;
    if (sinceLast >= 0 && sinceLast < minGap) {
      Serial.printf("changed, but last refresh was %lds ago - deferring "
                    "(min gap %lds)\n", sinceLast, minGap);
      sleepUntilNextCheck();
    }
  }

  // Rendering to an unpowered panel does nothing visible, but the code below
  // would still record the new content as shown and never retry. Try the
  // rails once more; if they still won't come up, leave the hash alone so the
  // next wake tries again.
  if (!gPanelPowered) gPanelPowered = pmuInitPanelRails();
  if (!gPanelPowered) {
    Serial.println("panel has no power - skipping refresh, will retry next wake");
    sleepUntilNextCheck();
  }

  Serial.printf("rendering (%d messages)\n", messageCount);
  digitalWrite(PIN_LED_GREEN, HIGH);
  renderBoard(haveTime ? &now : nullptr, false);
  digitalWrite(PIN_LED_GREEN, LOW);

  contentHash.toCharArray(rtcLastHash, sizeof(rtcLastHash));
  rtcLastRenderedYday = haveTime ? now.tm_yday : -1;
  rtcShowingOfflineNotice = false;
  if (haveTime) rtcLastRefreshEpoch = mktime(&now);

  sleepUntilNextCheck();
}

void loop() {
  // Never reached — setup() always ends in deep sleep.
}

// ===========================================================================
// Power
// ===========================================================================

/**
 * Waveshare's firmware drives the AXP2101 interrupt line as an output held
 * HIGH; letting it float or drop can signal the PMU to cut power. Hold it
 * through deep sleep so the frame actually wakes back up.
 */
void holdPmuRail() {
  gpio_hold_dis((gpio_num_t)PIN_PMU_IRQ);
  pinMode(PIN_PMU_IRQ, OUTPUT);
  digitalWrite(PIN_PMU_IRQ, HIGH);
}

/**
 * Read the battery before Wi-Fi comes up and loads it down, and build the
 * footer suffix from it.
 */
void readBattery() {
  const BatteryInfo bat = pmuReadBattery();
  if (!bat.present) {
    Serial.println("no battery detected (running on USB)");
    return;
  }
  Serial.printf("battery %d mV, %d%%%s\n", bat.millivolts, bat.percent,
                bat.charging ? ", charging" : bat.usb ? ", on USB" : "");

  // A fuel gauge that hasn't been switched on reads 0% regardless of charge.
  // Showing that would put a red "Battery low" on a full battery, so treat 0%
  // alongside a healthy voltage as unknown rather than trusting it.
  int percent = bat.percent;
  if (percent == 0 && bat.millivolts >= 3700) {
    Serial.println("fuel gauge reads 0% at a healthy voltage - ignoring it");
    percent = -1;
  }

  if (SHOW_BATTERY && percent >= 0) {
    // ASCII only — the footer font has no middot glyph. "charging" follows the
    // charger's own status, so a full battery on USB just shows its level.
    gBatteryNote = String(bat.charging ? "   -   charging " : "   -   ")
                 + String(percent) + "%";
    gBatteryLow = !bat.usb && percent < LOW_BATTERY_PERCENT;
  }
}

void blinkGreen(int times, int ms) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_LED_GREEN, HIGH);
    delay(ms);
    digitalWrite(PIN_LED_GREEN, LOW);
    delay(ms);
  }
}

/** Human-readable wake reason — tells us whether the timer actually fired. */
const char* wakeCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_EXT1:  return "button";
    case ESP_SLEEP_WAKEUP_EXT0:  return "ext0";
    case ESP_SLEEP_WAKEUP_GPIO:  return "gpio";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
      // Not a wake at all: power-on, reset button, or a crash/brownout reboot.
      return "POWER-ON or RESET (not a sleep wake)";
    default: return "other";
  }
}

void sleepUntilNextCheck() {
  display.hibernate();

  const uint64_t intervalSec = (uint64_t)(DEBUG_FAST_CYCLE ? DEBUG_INTERVAL_MINUTES
                                                           : CHECK_INTERVAL_MINUTES) * 60ULL;

  // Subtract the time this wake actually took, so the cadence stays on the
  // interval instead of drifting later by the length of every cycle. A render
  // costs ~25s on its own, which would otherwise push each cycle out.
  const uint64_t awakeSec = millis() / 1000ULL;
  uint64_t seconds = (awakeSec >= intervalSec) ? 30ULL : (intervalSec - awakeSec);

  Serial.printf("sleeping %llus\n", seconds);
  Serial.flush();

  digitalWrite(PIN_LED_GREEN, LOW);

  // Switch off every rail except the ESP32's own. This is the difference
  // between ~25 mA and ~1.2 mA asleep. Must come before the pin hold below,
  // and the PMU goes into its own sleep mode so the next wake can revive it.
  if (POWER_DOWN_IN_SLEEP) pmuPowerDownForSleep();

  // Keep the PMU line high while the CPU is powered down, so nothing wakes
  // the PMU by accident before the timer fires.
  gpio_hold_en((gpio_num_t)PIN_PMU_IRQ);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);

  // BOOT button = refresh now.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
  esp_sleep_enable_ext1_wakeup_io(1ULL << PIN_BTN_BOOT, ESP_EXT1_WAKEUP_ANY_LOW);
#else
  esp_sleep_enable_ext1_wakeup(1ULL << PIN_BTN_BOOT, ESP_EXT1_WAKEUP_ALL_LOW);
#endif

  esp_deep_sleep_start();
}

void handleFailure() {
  rtcFailureCount++;
  Serial.printf("failure #%lu\n", (unsigned long)rtcFailureCount);

  // Leave the last good board on screen for hours before admitting defeat —
  // a brief outage shouldn't wipe their messages. Skip it if the panel has no
  // power, so the notice is retried rather than recorded as shown.
  if (rtcFailureCount >= OFFLINE_NOTICE_AFTER && !rtcShowingOfflineNotice
      && gPanelPowered) {
    Serial.println("drawing offline notice");
    renderBoard(nullptr, true);
    rtcShowingOfflineNotice = true;
  }
}

// ===========================================================================
// Network
// ===========================================================================

bool connectWiFi() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - start > WIFI_TIMEOUT_MS) return false;
    delay(250);
  }
  Serial.printf("wifi ok, %s, rssi %d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

void syncClock() {
  configTzTime(TZ_STRING, NTP_SERVER, "time.nist.gov");
}

bool fetchMessages() {
  WiFiClientSecure client;
  // Apps Script serves a standard Google certificate. Pinning a root here
  // means a silent failure whenever Google rotates it, on a device with no
  // screen to explain itself — so we skip verification and rely on the
  // shared secret. The payload is family notes, not credentials.
  client.setInsecure();

  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  // Apps Script always 302s to script.googleusercontent.com.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  // Encode the secret so characters like & # + or a space can't break the
  // query string. Apps Script decodes it before comparing.
  String url = String(SCRIPT_URL) + "?key=" + urlEncode(SHARED_SECRET);
  if (!http.begin(client, url)) {
    Serial.println("http begin failed");
    return false;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("http %d\n", code);
    http.end();
    return false;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.printf("json: %s\n", err.c_str());
    return false;
  }

  if (doc["error"].is<const char*>()) {
    Serial.printf("server error: %s\n", doc["error"].as<const char*>());
    return false;
  }

  contentHash  = doc["hash"].as<String>();
  messageCount = 0;

  for (JsonObject m : doc["messages"].as<JsonArray>()) {
    if (messageCount >= MAX_MESSAGES) break;
    messages[messageCount].from   = toDisplayable(m["from"].as<String>());
    messages[messageCount].text   = toDisplayable(m["text"].as<String>());
    messages[messageCount].pinned = m["pinned"] | false;
    messageCount++;
  }

  Serial.printf("fetched %d messages, hash %s\n", messageCount, contentHash.c_str());
  return true;
}

// ===========================================================================
// Text helpers
// ===========================================================================

/**
 * The Adafruit FreeSans fonts only carry ASCII 0x20-0x7E. Email is full of
 * curly quotes, em dashes and accented names, all of which arrive as UTF-8
 * multibyte sequences and render as garbage boxes. Fold them down to ASCII.
 */
String toDisplayable(const String& in) {
  String out;
  out.reserve(in.length());

  for (size_t i = 0; i < in.length();) {
    uint8_t c = (uint8_t)in[i];

    if (c < 0x80) {                       // plain ASCII
      out += (char)c;
      i++;
      continue;
    }

    // Decode the code point so we can transliterate it.
    uint32_t cp = 0;
    int len = 1;
    if ((c & 0xE0) == 0xC0)      { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else { i++; continue; }               // stray continuation byte

    if (i + len > in.length()) break;
    for (int k = 1; k < len; k++) cp = (cp << 6) | ((uint8_t)in[i + k] & 0x3F);
    i += len;

    switch (cp) {
      case 0x2018: case 0x2019: case 0x201B: out += '\''; break;   // ' '
      case 0x201C: case 0x201D: case 0x201F: out += '"';  break;   // " "
      case 0x2013: case 0x2014: out += '-';  break;                // - -
      case 0x2026: out += "..."; break;                            // ...
      case 0x00A0: out += ' ';   break;                            // nbsp
      case 0x2022: out += '*';   break;                            // bullet
      default:
        if (cp == 0x200B || cp == 0x200C || cp == 0x200D ||        // zero-width, emoji joiner
            cp == 0x20E3 ||                                        // keycap
            (cp >= 0xFE00 && cp <= 0xFE0F) ||                      // variation selectors
            (cp >= 0x2300 && cp <= 0x23FF) ||                      // clocks, hourglass
            (cp >= 0x2600 && cp <= 0x27BF) ||                      // heart, sun, star, check
            (cp >= 0x2B00 && cp <= 0x2BFF)) {                      // stars, arrows
          // Emoji and symbols that live inside the 16-bit range. Without this,
          // a red heart (U+2764 + U+FE0F) shows up as "??".
        } else if (cp >= 0x00C0 && cp <= 0x00FF) {
          static const char* folded =
            "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
            "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
          out += folded[cp - 0x00C0];
        } else if (cp > 0xFFFF) {
          // Emoji and friends have no sensible fallback; drop them.
        } else {
          out += '?';
        }
        break;
    }
  }
  return out;
}

/** Percent-encode everything except the URL-safe characters. */
String urlEncode(const char* s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  for (; *s; s++) {
    const uint8_t c = (uint8_t)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

/** Width of a string in the currently selected font. */
int16_t textWidth(const String& s) {
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}

/**
 * Greedy word wrap. Returns the number of lines written into `lines`,
 * capped at maxLines; the last line gets an ellipsis if text was dropped.
 */
int wrapText(const String& text, int maxWidth, int maxLines, String* lines) {
  int count = 0;
  String current = "";
  int i = 0;

  while (i < (int)text.length() && count < maxLines) {
    // Pull the next word.
    int spaceAt = text.indexOf(' ', i);
    String word = (spaceAt == -1) ? text.substring(i) : text.substring(i, spaceAt);
    int nextI = (spaceAt == -1) ? text.length() : spaceAt + 1;

    String candidate = current.length() ? current + " " + word : word;

    if (textWidth(candidate) <= maxWidth) {
      current = candidate;
      i = nextI;
      continue;
    }

    if (current.length() == 0) {
      // Single word too long for the line — hard-break it.
      String chunk = "";
      while (i < (int)text.length()) {
        String trial = chunk + text[i];
        if (textWidth(trial) > maxWidth) break;
        chunk = trial;
        i++;
      }
      if (chunk.length() == 0) break;     // can't fit even one character
      lines[count++] = chunk;
    } else {
      lines[count++] = current;
      current = "";
    }
  }

  if (current.length() && count < maxLines) {
    lines[count++] = current;
  }

  // Signal truncation on the final line.
  if (i < (int)text.length() && count > 0) {
    String& last = lines[count - 1];
    while (last.length() && textWidth(last + "...") > maxWidth) {
      last.remove(last.length() - 1);
    }
    // Shrinking to fit can leave the line ending mid-gap, so trim after.
    while (last.length() && last[last.length() - 1] == ' ') {
      last.remove(last.length() - 1);
    }
    last += "...";
  }

  return count;
}


// ===========================================================================
// Rendering
// ===========================================================================

uint16_t accentFor(int index, bool pinned) {
  // Red is reserved for pinned messages, so a pin is never mistaken for an
  // ordinary message that happened to land on the red slot.
  if (pinned) return GxEPD_RED;
  static const uint16_t palette[] = { GxEPD_BLUE, GxEPD_GREEN, GxEPD_BLACK };
  return palette[index % 3];
}

void renderBoard(struct tm* now, bool offlineNotice) {
  display.init(115200, true, 2, false);
  // Remap SPI onto the PhotoPainter's pins (no MISO on this panel).
  SPI.end();
  SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);

  display.setRotation(SCREEN_ROTATION);
  display.setTextWrap(false);

  display.setFullWindow();
  display.firstPage();
  do {
    gHiddenCount = 0;   // drawMessages() sets it again on every page
    display.fillScreen(GxEPD_WHITE);
    drawHeader(now);
    if (offlineNotice) {
      drawOfflineNotice();
    } else if (messageCount == 0) {
      drawEmptyState();
    } else {
      drawMessages();
    }
    drawFooter(now, offlineNotice);
  } while (display.nextPage());

  display.hibernate();
}

void drawHeader(struct tm* now) {
  display.fillRect(0, 0, EPD_WIDTH, HEADER_H, GxEPD_BLUE);

  display.setTextColor(GxEPD_WHITE);
  display.setFont(&FreeSansBold18pt7b);
  display.setCursor(MARGIN, 47);
  display.print(BOARD_TITLE);

  if (now) {
    // %e and %l are GNU extensions that newlib doesn't reliably provide,
    // so build the un-padded forms by hand.
    char dateBuf[48];
    strftime(dateBuf, sizeof(dateBuf), "%A, %B", now);
    String date = String(dateBuf) + " " + String(now->tm_mday);

    display.setFont(&FreeSans12pt7b);
    display.setCursor(EPD_WIDTH - MARGIN - textWidth(date), 45);
    display.print(date);
  }
}

void drawMessages() {
  int y = CONTENT_TOP;

  for (int i = 0; i < messageCount; i++) {
    // Measure this message before committing to drawing it.
    display.setFont(&FreeSans18pt7b);
    String lines[4];
    const int lineHeight = 34;
    int lineCount = wrapText(messages[i].text, CONTENT_W - 18, 4, lines);

    const int senderH = 24;
    const int blockH  = senderH + (lineCount * lineHeight) + 16;

    // Only the visible part has to fit: the accent bar ends 12 px above the
    // bottom of the block, and the gap below it is spacing for the next
    // message. Counting that gap rejected two 4-line messages by 4 px.
    if (y + blockH - 12 > CONTENT_BOTTOM) {
      // Out of room. The footer says how many didn't fit, because there may
      // be no space left here to say it without running into the footer.
      gHiddenCount = messageCount - i;
      return;
    }

    const uint16_t accent = accentFor(i, messages[i].pinned);

    // Accent bar down the left edge of the message.
    display.fillRect(MARGIN, y, 6, blockH - 12, accent);

    // Sender
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(accent);
    display.setCursor(MARGIN + 18, y + 18);
    display.print(messages[i].from);
    if (messages[i].pinned) {
      display.print("  *");
    }

    // Body
    display.setFont(&FreeSans18pt7b);
    display.setTextColor(GxEPD_BLACK);
    int lineY = y + senderH + 24;
    for (int l = 0; l < lineCount; l++) {
      display.setCursor(MARGIN + 18, lineY);
      display.print(lines[l]);
      lineY += lineHeight;
    }

    y += blockH;
  }
}

void drawEmptyState() {
  display.setFont(&FreeSans18pt7b);
  display.setTextColor(GxEPD_BLACK);

  const String line1 = "No messages right now.";
  const String line2 = "Check back later!";

  display.setCursor((EPD_WIDTH - textWidth(line1)) / 2, 250);
  display.print(line1);

  display.setFont(&FreeSans12pt7b);
  display.setTextColor(GxEPD_BLUE);
  display.setCursor((EPD_WIDTH - textWidth(line2)) / 2, 295);
  display.print(line2);
}

void drawOfflineNotice() {
  display.setFont(&FreeSansBold18pt7b);
  display.setTextColor(GxEPD_RED);
  const String line1 = "Not connected";
  display.setCursor((EPD_WIDTH - textWidth(line1)) / 2, 235);
  display.print(line1);

  display.setFont(&FreeSans12pt7b);
  display.setTextColor(GxEPD_BLACK);
  const String line2 = "The frame can't reach the internet.";
  const String line3 = "New messages will appear when it reconnects.";
  display.setCursor((EPD_WIDTH - textWidth(line2)) / 2, 285);
  display.print(line2);
  display.setCursor((EPD_WIDTH - textWidth(line3)) / 2, 315);
  display.print(line3);
}

void drawFooter(struct tm* now, bool offlineNotice) {
  const int y = EPD_HEIGHT - FOOTER_H;
  display.drawFastHLine(MARGIN, y, EPD_WIDTH - (2 * MARGIN), GxEPD_BLACK);

  display.setFont(&FreeSans9pt7b);
  // A flat battery is the one failure the reader can actually fix, so it takes
  // priority over the offline notice for the footer colour.
  display.setTextColor((offlineNotice || gBatteryLow) ? GxEPD_RED : GxEPD_BLACK);
  display.setCursor(MARGIN, y + 24);

  if (now) {
    int hour12 = now->tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%d:%02d %s",
             hour12, now->tm_min, now->tm_hour < 12 ? "AM" : "PM");

    display.print(offlineNotice ? "Last tried " : "Updated ");
    display.print(timeBuf);
  } else {
    display.print("Updated");
  }

  // ASCII only below: FreeSans9pt7b carries 0x20-0x7E, so a middot or em dash
  // would come out as a garbage box.
  if (gBatteryLow) {
    display.print("   -   Battery low, please charge");
  }
  if (gBatteryNote.length()) {
    display.print(gBatteryNote);
  }

  // Right-hand side: how many messages didn't fit, then the debug tag.
  String right;
  if (gHiddenCount > 0) {
    // Short form in debug mode: with a low-battery warning on the left and the
    // debug tag on the right, the long form runs into the left-hand text
    // (checked against real glyph widths in tests/test_footer_width.py).
    right = String("+") + String(gHiddenCount)
          + (DEBUG_FAST_CYCLE ? " more"
                              : (gHiddenCount == 1 ? " more message" : " more messages"));
  }

  if (DEBUG_FAST_CYCLE) {
    // Proof the board is cycling, visible without a serial connection.
    // Seconds are included so two consecutive checks are obviously different.
    String tag = "no clock";
    if (now) {
      int hour12 = now->tm_hour % 12;
      if (hour12 == 0) hour12 = 12;
      char buf[40];
      snprintf(buf, sizeof(buf), "checked %d:%02d:%02d %s",
               hour12, now->tm_min, now->tm_sec,
               now->tm_hour < 12 ? "AM" : "PM");
      tag = String(buf);
    }
    right = right.length() ? right + "   " + tag : tag;
  }

  if (right.length()) {
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(EPD_WIDTH - MARGIN - textWidth(right), y + 24);
    display.print(right);
  }
}
