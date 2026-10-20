// pmu.h — AXP2101 power management for the PhotoPainter.
//
// The board's rails:
//   DCDC1        the ESP32 itself. Never touched.
//   ALDO3/ALDO4  the e-paper panel. Without them the panel ignores every
//                command and GxEPD2 reports a refresh of a few µs.
//   the rest     audio codec and amplifier, SD card, sensors. Not used by
//                this firmware, but on from power-up unless switched off.
//
// Leaving all of those on through deep sleep costs roughly 25 mA, which flattens
// the 1,500 mAh battery in a couple of days. Waveshare's own power-consumption
// test (04_PowerConsumptionTest in their repo) switches off every rail except
// DCDC1 and measures about 1.2 mA. pmuPowerDownForSleep() does the same.
//
// It has to go through the chip's own sleep mode, not just switch the rails off.
// With the rails simply off, the unpowered chips sharing the I2C bus hold it low
// and the ESP32 can't reach the PMU on the next wake to turn anything back on.
// So: put the AXP2101 in sleep mode first, configured to wake when its IRQ line
// (GPIO21) is pulled low and to restore the rails itself. pmuBegin() sends that
// pulse if the chip doesn't answer.
//
// Register addresses and bit positions verified against the XPowersLib copy
// vendored in waveshareteam/ESP32-S3-PhotoPainter.

#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>
#include "board_pins.h"

#define AXP2101_ADDR            0x34
#define AXP2101_STATUS1         0x00   // bit3 = battery present, bit5 = USB power good
#define AXP2101_STATUS2         0x01   // bits 6:5 = 01 when charging
#define AXP2101_SLEEP_WAKE      0x26   // bit0 = enter sleep, bit2/3/4 = wake behaviour
#define AXP2101_ADC_CTRL        0x30   // bit0 = battery voltage measurement
#define AXP2101_ADC_VBAT_H      0x34   // battery mV, 14 bits across 0x34/0x35
#define AXP2101_ADC_VBAT_L      0x35
#define AXP2101_IRQ_EN0         0x40   // 0x40..0x42 interrupt enables
#define AXP2101_IRQ_STS0        0x48   // 0x48..0x4A interrupt status (write 1 to clear)
#define AXP2101_BAT_DET_CTRL    0x68   // bit0 = battery detection
#define AXP2101_DC_ONOFF        0x80   // bit0 = DCDC1 (ESP32) ... bit4 = DCDC5
#define AXP2101_LDO_ONOFF_CTRL0 0x90   // ALDO1-4, BLDO1-2, CPUSLDO, DLDO1
#define AXP2101_LDO_ONOFF_CTRL1 0x91   // bit0 = DLDO2
#define AXP2101_LDO_VOL2_CTRL   0x94   // ALDO3 voltage
#define AXP2101_LDO_VOL3_CTRL   0x95   // ALDO4 voltage
#define AXP2101_BAT_PERCENT     0xA4   // fuel gauge, 0-100
#define AXP2101_VOL_3V3         0x1C   // 0.5 V + 28 × 0.1 V

inline bool pmuReadRegister(uint8_t reg, uint8_t& value) {
  Wire.beginTransmission(AXP2101_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((uint8_t)AXP2101_ADDR, (uint8_t)1) != 1) return false;
  value = Wire.read();
  return true;
}

inline bool pmuWriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(AXP2101_ADDR);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

inline bool pmuSetBits(uint8_t reg, uint8_t mask) {
  uint8_t v;
  if (!pmuReadRegister(reg, v)) return false;
  return (v & mask) == mask || pmuWriteRegister(reg, v | mask);
}

inline bool pmuProbe(uint32_t hz) {
  Wire.end();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, hz);
  delay(10);
  uint8_t v;
  return pmuReadRegister(AXP2101_STATUS1, v);
}

// Pull the AXP2101's IRQ line low for `ms`, then back high. While the chip is
// in sleep mode this wakes it, and it switches its rails back on.
inline void pmuPulseIrq(uint32_t ms) {
  gpio_hold_dis((gpio_num_t)PIN_PMU_IRQ);
  pinMode(PIN_PMU_IRQ, OUTPUT);
  digitalWrite(PIN_PMU_IRQ, LOW);
  delay(ms);
  digitalWrite(PIN_PMU_IRQ, HIGH);
  delay(150);   // rails coming up
}

// Whether the PMU has already answered this boot. Wrapped in a function so the
// host-side test (tests/test_pmu.cpp) can reset it between cases; in normal use
// it is just a static flag that avoids re-probing on every call.
inline bool& pmuCachedOk() { static bool ok = false; return ok; }
inline void pmuResetCachedOk() { pmuCachedOk() = false; }

// Start I2C and make sure the chip answers, waking it if it was left asleep.
inline bool pmuBegin() {
  bool& ok = pmuCachedOk();
  if (ok) return true;
  if (pmuProbe(400000)) return ok = true;
  static const uint32_t pulses[] = {50, 300, 1000};
  for (uint32_t ms : pulses) {
    Serial.printf("PMU: not answering, waking it (%lu ms pulse)\n", (unsigned long)ms);
    pmuPulseIrq(ms);
    // If the board's own I2C pull-ups were on a rail that was switched off,
    // the ESP32's internal pull-ups still work, just not at 400 kHz.
    if (pmuProbe(400000) || pmuProbe(100000)) return ok = true;
  }
  Serial.println("PMU: no response at 0x34");
  return false;
}

/**
 * Bring up the rails the panel needs. Safe to call on every wake — it only
 * writes when a register isn't already correct.
 */
inline bool pmuInitPanelRails() {
  if (!pmuBegin()) return false;

  uint8_t onoff = 0;
  if (!pmuReadRegister(AXP2101_LDO_ONOFF_CTRL0, onoff)) return false;

  uint8_t vol = 0;
  if (pmuReadRegister(AXP2101_LDO_VOL2_CTRL, vol) && (vol & 0x1F) != AXP2101_VOL_3V3)
    pmuWriteRegister(AXP2101_LDO_VOL2_CTRL, (vol & 0xE0) | AXP2101_VOL_3V3);
  if (pmuReadRegister(AXP2101_LDO_VOL3_CTRL, vol) && (vol & 0x1F) != AXP2101_VOL_3V3)
    pmuWriteRegister(AXP2101_LDO_VOL3_CTRL, (vol & 0xE0) | AXP2101_VOL_3V3);

  const uint8_t wanted = onoff | (1 << 2) | (1 << 3);   // ALDO3 + ALDO4
  if (wanted != onoff) {
    if (!pmuWriteRegister(AXP2101_LDO_ONOFF_CTRL0, wanted)) {
      Serial.println("PMU: failed to enable ALDO3/ALDO4");
      return false;
    }
    Serial.println("PMU: enabled panel rails");
    delay(100);
  }
  return true;
}

struct BatteryInfo {
  bool present = false;
  bool usb = false;        // running from USB power
  bool charging = false;
  int  millivolts = 0;
  int  percent = -1;       // fuel gauge; -1 if unknown
};

inline BatteryInfo pmuReadBattery() {
  BatteryInfo b;
  if (!pmuBegin()) return b;
  pmuSetBits(AXP2101_ADC_CTRL, 0x01);       // battery voltage measurement on
  pmuSetBits(AXP2101_BAT_DET_CTRL, 0x01);   // battery detection on
  uint8_t s1 = 0, s2 = 0, h = 0, l = 0, pct = 0;
  if (!pmuReadRegister(AXP2101_STATUS1, s1)) return b;
  pmuReadRegister(AXP2101_STATUS2, s2);
  b.present  = s1 & (1 << 3);
  b.usb      = s1 & (1 << 5);
  b.charging = ((s2 >> 5) & 0x03) == 0x01;
  if (b.present) {
    if (pmuReadRegister(AXP2101_ADC_VBAT_H, h) && pmuReadRegister(AXP2101_ADC_VBAT_L, l))
      b.millivolts = ((h & 0x3F) << 8) | l;
    if (pmuReadRegister(AXP2101_BAT_PERCENT, pct) && pct <= 100) b.percent = pct;
  }
  return b;
}

/**
 * Put the AXP2101 to sleep with everything except DCDC1 (the ESP32's own
 * supply) switched off. Same order as Waveshare's axp_basic_sleep_start().
 */
inline bool pmuPowerDownForSleep() {
  if (!pmuBegin()) return false;

  // No interrupts: the chip must not pull its own IRQ line, since that line
  // is what we use to wake it.
  for (uint8_t i = 0; i < 3; i++) pmuWriteRegister(AXP2101_IRQ_EN0 + i, 0x00);
  for (uint8_t i = 0; i < 3; i++) pmuWriteRegister(AXP2101_IRQ_STS0 + i, 0xFF);

  uint8_t w = 0;
  if (!pmuReadRegister(AXP2101_SLEEP_WAKE, w)) return false;
  w |= (1 << 2);              // on wake, rails as they were before sleep
  w &= ~(1 << 3);             // don't pull PWROK low on wake (that would reset the ESP32)
  w |= (1 << 4);              // wake when the IRQ pin is pulled low
  if (!pmuWriteRegister(AXP2101_SLEEP_WAKE, w)) return false;
  if (!pmuWriteRegister(AXP2101_SLEEP_WAKE, w | 0x01)) return false;   // enter sleep mode

  uint8_t dc = 0;
  if (pmuReadRegister(AXP2101_DC_ONOFF, dc)) pmuWriteRegister(AXP2101_DC_ONOFF, dc & 0x01);
  pmuWriteRegister(AXP2101_LDO_ONOFF_CTRL0, 0x00);
  uint8_t l1 = 0;
  if (pmuReadRegister(AXP2101_LDO_ONOFF_CTRL1, l1))
    pmuWriteRegister(AXP2101_LDO_ONOFF_CTRL1, l1 & ~0x01);

  Serial.println("PMU: asleep, rails off");
  return true;
}
