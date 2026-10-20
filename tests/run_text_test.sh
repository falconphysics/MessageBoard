#!/usr/bin/env bash
# Run the text tests against the REAL functions in MessageBoard.ino.
#
# toDisplayable(), urlEncode() and wrapText() are cut out of the sketch into
# text_body.inc, so the test always exercises the shipping code.
set -euo pipefail
cd "$(dirname "$0")"

SRC=../firmware/MessageBoard/MessageBoard.ino
OUT=text_body.inc

: > "$OUT"
for fn in '^String toDisplayable\(' '^String urlEncode\(' '^int wrapText\('; do
  # From the function's first line to the first line that is just "}".
  awk -v pat="$fn" '$0 ~ pat {on=1} on {print} on && /^}/ {exit}' "$SRC" >> "$OUT"
  echo >> "$OUT"
done
grep -q 'toDisplayable' "$OUT" && grep -q 'urlEncode' "$OUT" && grep -q 'wrapText' "$OUT" \
  || { echo "could not extract the functions from $SRC"; exit 1; }

g++ -std=c++17 -Wall -o test_text test_text.cpp
./test_text
