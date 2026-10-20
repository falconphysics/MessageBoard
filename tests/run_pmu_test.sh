#!/usr/bin/env bash
# Run the PMU test against the REAL pmu.h.
#
# pmu.h is generated into pmu_body.inc with its Arduino includes stripped, so
# the test always exercises the shipping code rather than a stale copy.
set -euo pipefail
cd "$(dirname "$0")"

SRC=../firmware/MessageBoard/pmu.h
OUT=pmu_body.inc

sed -e '/^#pragma once/d' \
    -e '/^#include <Arduino.h>/d' \
    -e '/^#include <Wire.h>/d' \
    -e '/^#include <driver\/gpio.h>/d' \
    -e '/^#include "board_pins.h"/d' \
    "$SRC" > "$OUT"

g++ -std=c++17 -Wall -I. -I../firmware/MessageBoard test_pmu.cpp -o test_pmu
./test_pmu
