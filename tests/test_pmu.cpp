// Host-side test for pmu.h against a simulated AXP2101.
//
// The sleep path is the risky part: a wrong bit in register 0x26 leaves the
// chip with its rails off and no way to wake it, which on real hardware means
// disconnecting USB *and* the battery. This exercises it without hardware.
//
// Build: g++ -std=c++17 -I. -I../firmware/MessageBoard test_pmu.cpp -o test_pmu

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
#include <string>

// ---- Arduino / Wire stubs --------------------------------------------------

static void delay(unsigned long) {}

struct SerialStub {
  void println(const char* s) { ::printf("    [serial] %s\n", s); }
  // NOTE: ::printf, not printf — an unqualified call here resolves to this
  // member and recurses until the stack blows.
  template <typename... A> void printf(const char* f, A... a) {
    char buf[256];
    snprintf(buf, sizeof(buf), f, a...);
    ::printf("    [serial] %s", buf);
  }
} Serial;

// A simulated AXP2101 whose registers we can inspect.
struct FakeChip {
  std::map<uint8_t, uint8_t> reg;
  bool asleep = false;          // set once sleep bit written
  bool railsOff = false;
  bool reachable = true;        // false simulates the I2C bus held low
  int  irqPulses = 0;

  void reset() {
    reg.clear();
    // Power-on defaults: everything on.
    reg[0x00] = (1 << 3) | (1 << 5);   // battery present + USB good
    reg[0x01] = (0x01 << 5);           // charging
    reg[0x26] = 0x00;
    reg[0x30] = 0x00;
    reg[0x34] = 0x0F; reg[0x35] = 0xA0;
    reg[0x68] = 0x00;
    reg[0x80] = 0x1F;                  // DCDC1..5 on
    reg[0x90] = 0xFF;
    reg[0x91] = 0x01;
    reg[0x94] = 0x00; reg[0x95] = 0x00;
    reg[0xA4] = 82;
    asleep = false; railsOff = false; reachable = true; irqPulses = 0;
  }
} chip;

static int wireFreq = 0;
static bool wireBegun = false;

struct WireStub {
  uint8_t pendingReg = 0;
  bool haveReg = false;
  std::vector<uint8_t> rx;

  void end() { wireBegun = false; }
  void begin(int, int, int hz) { wireBegun = true; wireFreq = hz; }
  void beginTransmission(uint8_t) { haveReg = false; }
  void write(uint8_t v) {
    if (!haveReg) { pendingReg = v; haveReg = true; }
    else {
      if (chip.reachable) chip.reg[pendingReg] = v;
      pendingVal = v; wroteValue = true;
    }
  }
  int endTransmission(bool = true) {
    if (!chip.reachable) return 2;
    if (wroteValue) {
      // Writing the sleep bit puts the chip under.
      if (pendingReg == 0x26 && (pendingVal & 0x01)) chip.asleep = true;
      if (pendingReg == 0x90 && pendingVal == 0x00) chip.railsOff = true;
      wroteValue = false;
    }
    return 0;
  }
  int requestFrom(uint8_t, uint8_t n) {
    if (!chip.reachable) return 0;
    rx.clear();
    for (uint8_t i = 0; i < n; i++) rx.push_back(chip.reg[pendingReg]);
    return n;
  }
  uint8_t read() { uint8_t v = rx.front(); rx.erase(rx.begin()); return v; }

  uint8_t pendingVal = 0;
  bool wroteValue = false;
} Wire;

// GPIO stubs — a LOW pulse on the IRQ pin wakes the simulated chip.
#define OUTPUT 1
#define LOW 0
#define HIGH 1
typedef int gpio_num_t;
static void gpio_hold_dis(gpio_num_t) {}
static void pinMode(int, int) {}
static int lastIrqLevel = HIGH;
static void digitalWrite(int pin, int level);

#define PIN_I2C_SDA 47
#define PIN_I2C_SCL 48
#define PIN_PMU_IRQ 21

static void digitalWrite(int pin, int level) {
  if (pin != PIN_PMU_IRQ) return;
  if (lastIrqLevel == LOW && level == HIGH) {
    chip.irqPulses++;
    if (chip.asleep) {           // wake: chip restores its own rails
      chip.asleep = false;
      chip.railsOff = false;
      chip.reachable = true;
      chip.reg[0x90] = 0xFF;
      chip.reg[0x80] = 0x1F;
    }
  }
  lastIrqLevel = level;
}

// ---- the code under test ---------------------------------------------------
#define PMU_TEST_NO_BOARD_PINS 1
#include "pmu_body.inc"

// ---- tests -----------------------------------------------------------------

static int failures = 0;
static void check(bool cond, const char* what) {
  printf("  %s %s\n", cond ? "ok  " : "FAIL", what);
  if (!cond) failures++;
}

static void resetAll() {
  chip.reset();
  pmuResetCachedOk();
  lastIrqLevel = HIGH;
}

int main() {
  printf("=== panel rails come up ===\n");
  resetAll();
  chip.reg[0x90] = 0x00;                    // rails off, as after a sleep
  check(pmuInitPanelRails(), "pmuInitPanelRails succeeds");
  check((chip.reg[0x90] & (1 << 2)) != 0, "ALDO3 enabled");
  check((chip.reg[0x90] & (1 << 3)) != 0, "ALDO4 enabled");
  check((chip.reg[0x94] & 0x1F) == 0x1C, "ALDO3 set to 3.3V");
  check((chip.reg[0x95] & 0x1F) == 0x1C, "ALDO4 set to 3.3V");

  printf("\n=== sleep configures wake correctly ===\n");
  resetAll();
  check(pmuPowerDownForSleep(), "pmuPowerDownForSleep succeeds");
  const uint8_t w = chip.reg[0x26];
  check((w & (1 << 4)) != 0, "bit4 set: wake on IRQ pin low");
  check((w & (1 << 2)) != 0, "bit2 set: restore rails on wake");
  check((w & (1 << 3)) == 0, "bit3 CLEAR: must not pull PWROK low (would reset ESP32)");
  check((w & 0x01) != 0,     "bit0 set: sleep entered");
  check(chip.asleep,         "chip is asleep");

  printf("\n=== sleep leaves DCDC1 on, everything else off ===\n");
  check((chip.reg[0x80] & 0x01) != 0, "DCDC1 (ESP32 supply) still on");
  check((chip.reg[0x80] & 0x1E) == 0, "DCDC2-5 off");
  check(chip.reg[0x90] == 0x00,       "ALDO1-4/BLDO/CPUSLDO/DLDO1 off");
  check((chip.reg[0x91] & 0x01) == 0, "DLDO2 off");

  printf("\n=== interrupts disabled so the chip can't pull its own IRQ ===\n");
  check(chip.reg[0x40] == 0x00 && chip.reg[0x41] == 0x00 && chip.reg[0x42] == 0x00,
        "IRQ enables cleared");

  printf("\n=== wake from a bus held low ===\n");
  // After sleep the other chips are unpowered and hold I2C low.
  chip.reachable = false;
  pmuResetCachedOk();
  const int before = chip.irqPulses;
  const bool woke = pmuBegin();
  check(woke, "pmuBegin recovers the chip");
  check(chip.irqPulses > before, "an IRQ pulse was sent");
  check(!chip.asleep, "chip is awake again");
  check(pmuInitPanelRails(), "panel rails can be brought up after the wake");
  check((chip.reg[0x90] & 0x0C) == 0x0C, "ALDO3+ALDO4 on after wake");

  printf("\n=== battery reading ===\n");
  resetAll();
  BatteryInfo b = pmuReadBattery();
  check(b.present, "battery detected");
  check(b.usb, "USB power detected");
  check(b.charging, "charging detected");
  check(b.percent == 82, "percent read from fuel gauge");
  check(b.millivolts == ((0x0F & 0x3F) << 8 | 0xA0), "millivolts assembled from 0x34/0x35");
  check((chip.reg[0x30] & 0x01) != 0, "battery voltage ADC enabled");
  check((chip.reg[0x68] & 0x01) != 0, "battery detection enabled");

  printf("\n=== no battery fitted ===\n");
  resetAll();
  chip.reg[0x00] = (1 << 5);        // USB only, no battery
  b = pmuReadBattery();
  check(!b.present, "reports no battery");
  check(b.percent == -1, "percent stays unknown");

  printf("\n=== unreachable chip fails safely ===\n");
  resetAll();
  chip.reachable = false;
  chip.asleep = false;              // not asleep, genuinely broken
  pmuResetCachedOk();
  check(!pmuInitPanelRails(), "pmuInitPanelRails returns false, does not hang");
  pmuResetCachedOk();
  b = pmuReadBattery();
  check(!b.present, "battery read returns empty, does not hang");

  printf("\n%s (%d failures)\n", failures ? "TESTS FAILED" : "ALL TESTS PASSED", failures);
  return failures ? 1 : 0;
}
