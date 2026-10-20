/*
 * PanelTest — bring-up sketch for the ESP32-S3-PhotoPainter panel.
 *
 * No Wi-Fi, no Gmail, no deep sleep. It does the smallest possible thing that
 * should make the screen change, and prints timings and the raw BUSY pin level
 * at each step so we can see where it stops.
 *
 * What to look for:
 *   - clearScreen should take roughly 20 SECONDS and leave the panel white.
 *   - If clearScreen returns in microseconds, the panel isn't accepting the
 *     refresh command and the problem is below the drawing code.
 *
 * Board settings are the same as the main sketch (ESP32S3 Dev Module, 16MB
 * flash, OPI PSRAM, USB CDC On Boot enabled).
 */

#include <SPI.h>
#include <Wire.h>
#include <GxEPD2_7C.h>

#include "board_pins.h"
#include "pmu.h"
#include "panel.h"

// Flip this to try the older 7-colour classes if GDEP073E01 turns out wrong.
//   1 = GDEP073E01   (Spectra 6 / "E6" / Waveshare 7.3" E)   <-- expected
//   2 = GDEY073D46   (older 7-colour ACeP)
//   3 = ACeP_730     (older 7-colour ACeP, alternate init)
#define PANEL_CHOICE 1

#if   PANEL_CHOICE == 1
  #define PANEL_CLASS PhotoPainterPanel
#elif PANEL_CHOICE == 2
  #define PANEL_CLASS GxEPD2_730c_GDEY073D46
#else
  #define PANEL_CLASS GxEPD2_730c_ACeP_730
#endif

GxEPD2_7C<PANEL_CLASS, PANEL_CLASS::HEIGHT / 4> display(
    PANEL_CLASS(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));

void showBusy(const char* when) {
  Serial.printf("  BUSY pin (GPIO%d) = %s   [%s]\n",
                PIN_EPD_BUSY,
                digitalRead(PIN_EPD_BUSY) ? "HIGH (idle)" : "LOW (busy)",
                when);
}

void setup() {
  Serial.begin(115200);
  delay(3000);            // give the USB CDC port time to enumerate

  Serial.println("\n\n========== PhotoPainter panel test ==========");
  Serial.printf("Panel class: %s\n",
#if PANEL_CHOICE == 1
    "PhotoPainterPanel (GDEP073E01, 45s busy timeout)"
#elif PANEL_CHOICE == 2
    "GxEPD2_730c_GDEY073D46"
#else
    "GxEPD2_730c_ACeP_730"
#endif
  );

  // Hold the PMU interrupt line high, same as the main sketch.
  pinMode(PIN_PMU_IRQ, OUTPUT);
  digitalWrite(PIN_PMU_IRQ, HIGH);

  Serial.println("\n--- PMU ---");
  pmuInitPanelRails();

  pinMode(PIN_EPD_BUSY, INPUT);
  showBusy("before display.init");

  Serial.println("\n--- display.init ---");
  display.init(115200, true, 2, false);
  SPI.end();
  SPI.begin(PIN_EPD_SCK, -1, PIN_EPD_MOSI, PIN_EPD_CS);
  showBusy("after display.init");

  // ---- Test 1: clearScreen -------------------------------------------------
  // This is the simplest operation the panel supports. It should blank the
  // screen to white and take about 20 seconds.
  Serial.println("\n--- TEST 1: clearScreen() ---");
  Serial.println("Expect ~20 seconds and a white screen. Watch the panel.");
  uint32_t t0 = millis();
  display.clearScreen();
  uint32_t elapsed = millis() - t0;
  Serial.printf(">>> clearScreen took %lu ms\n", (unsigned long)elapsed);
  showBusy("after clearScreen");

  if (elapsed < 3000) {
    Serial.println(">>> FAILED: too fast. The panel is not executing refreshes.");
    Serial.println(">>> Try PANEL_CHOICE 2 or 3, or check EPD wiring.");
  } else {
    Serial.println(">>> Looks right. Panel is refreshing.");
  }

  delay(2000);

  // ---- Test 2: colour bars via the paged path ------------------------------
  // Same drawing path the real sketch uses, so if test 1 passes and this
  // fails, the bug is in the rendering code rather than the panel.
  Serial.println("\n--- TEST 2: colour bars (paged full window) ---");
  const uint16_t colors[6] = {
    GxEPD_BLACK, GxEPD_WHITE, GxEPD_RED,
    GxEPD_YELLOW, GxEPD_BLUE, GxEPD_GREEN
  };

  t0 = millis();
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    for (int i = 0; i < 6; i++) {
      display.fillRect(i * (800 / 6), 0, 800 / 6, 380, colors[i]);
    }
    // A black bar along the bottom proves the last page renders too.
    display.fillRect(0, 420, 800, 60, GxEPD_BLACK);
  } while (display.nextPage());
  Serial.printf(">>> colour bars took %lu ms\n", (unsigned long)(millis() - t0));

  Serial.println("\nYou should see six vertical colour bars with a black strip");
  Serial.println("along the bottom. If the bottom strip is missing, paging is");
  Serial.println("the problem. If the whole screen is unchanged, the panel is.");

  display.hibernate();
  Serial.println("\n========== done (staying awake) ==========");
}

void loop() {
  delay(10000);
}
