#!/usr/bin/env python3
"""
Check the footer still fits across the panel now that the battery level has
been appended, using the REAL glyph advance widths from Adafruit's
FreeSans9pt7b.h rather than an estimate.

Also verifies every character the footer can emit is inside the font's
0x20-0x7E range, since anything outside it renders as a garbage box.
"""
import os
import re
import sys

PANEL_W = 800
MARGIN = 28
USABLE = PANEL_W - 2 * MARGIN


def load_advances(path):
    """Return {codepoint: xAdvance} parsed from the GFXglyph table."""
    src = open(path).read()
    block = src[src.index("Glyphs[] PROGMEM = {"):]
    block = block[:block.index("};")]
    # Each entry: {bitmapOffset, width, height, xAdvance, xOffset, yOffset}
    entries = re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,", block)
    first, last = 0x20, 0x7E
    assert len(entries) == last - first + 1, f"{len(entries)} glyphs, expected {last-first+1}"
    return {first + i: int(e[3]) for i, e in enumerate(entries)}, first, last


def width_of(text, adv, first, last):
    total = 0
    for ch in text:
        cp = ord(ch)
        if cp < first or cp > last:
            return None, ch          # outside the font
        total += adv[cp]
    return total, None


def main():
    # Usage: python3 test_footer_width.py [path/to/FreeSans9pt7b.h]
    # Default: FreeSans9pt7b.h in this folder. Copy it there from the Adafruit
    # GFX library (Adafruit_GFX_Library/Fonts/FreeSans9pt7b.h).
    font_h = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "FreeSans9pt7b.h")
    if not os.path.exists(font_h):
        print(f"Cannot find {font_h}")
        print("Copy FreeSans9pt7b.h into the tests folder, or pass its path as an argument.")
        return 2
    adv, first, last = load_advances(font_h)
    print(f"font covers 0x{first:02X}-0x{last:02X}\n")

    # Every footer string the firmware can produce, worst case values.
    cases = [
        ("normal",            "Updated 12:34 PM"),
        ("offline",           "Last tried 12:34 PM"),
        ("with battery",      "Updated 12:34 PM   -   82%"),
        ("charging",          "Updated 12:34 PM   -   charging 100%"),
        ("low battery",       "Updated 12:34 PM   -   Battery low, please charge   -   14%"),
        ("offline + low",     "Last tried 12:34 PM   -   Battery low, please charge   -   9%"),
        ("no clock + low",    "Updated   -   Battery low, please charge   -   9%"),
    ]
    # Offline cases replace the messages, so nothing is ever hidden alongside
    # them; only the others can share the line with "+N more".
    with_messages = {"normal", "with battery", "charging", "low battery", "no clock + low"}

    # Right-aligned on the same line: messages that didn't fit, then the
    # debug tag. Worst cases for width.
    right_cases = [
        ("more",              "+7 more messages"),
        ("debug tag",         "checked 12:34:56 PM"),
        # Debug mode shortens the note to "+N more" so both fit.
        ("more + debug",      "+7 more   checked 12:34:56 PM"),
    ]

    failures = 0
    left_w = {}
    for name, text in cases + right_cases:
        w, bad = width_of(text, adv, first, last)
        if bad is not None:
            print(f"  FAIL {name:16s} non-ASCII char {bad!r} (U+{ord(bad):04X}) not in font")
            failures += 1
            continue
        fits = w <= USABLE
        if name in dict(cases):
            left_w[name] = w
        print(f"  {'ok  ' if fits else 'FAIL'} {name:16s} {w:4d}px / {USABLE}px  |{text}|")
        if not fits:
            failures += 1

    # The two sides must never meet.
    print()
    for rname, rtext in right_cases:
        rw, _ = width_of(rtext, adv, first, last)
        pool = left_w if rname == "debug tag" else {k: v for k, v in left_w.items() if k in with_messages}
        lname, lw = max(pool.items(), key=lambda kv: kv[1])
        gap = USABLE - lw - rw
        ok = gap >= 8
        print(f"  {'ok  ' if ok else 'FAIL'} overlap {rname:14s} vs {lname:16s} {lw}+{rw}px, {gap:+d}px of room")
        if not ok:
            failures += 1

    print(f"\n{'TESTS FAILED' if failures else 'ALL TESTS PASSED'} ({failures} failures)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
