#!/usr/bin/env python3
"""
Render a to-scale preview of the message board layout (800x480), using the
same geometry constants and the same six-color palette as the firmware.
Useful for judging type size before flashing anything.
"""

W, H = 800, 480
MARGIN = 28
HEADER_H = 72
FOOTER_H = 34
CONTENT_TOP = HEADER_H + 16
CONTENT_BOTTOM = H - FOOTER_H - 10
CONTENT_W = W - 2 * MARGIN

# Spectra 6 palette
BLACK, WHITE = "#000000", "#FFFFFF"
RED, YELLOW = "#DC3220", "#E8C51D"
BLUE, GREEN = "#1F4E9C", "#2E7D32"

# Red is reserved for pinned messages, as in the firmware.
ACCENTS = [BLUE, GREEN, BLACK]

import sys

MESSAGES = [
    ("Alex", "Dinner at our place Sunday at 5. I'll pick you both up at 4:30.", True),
    ("Sam", "The kids made you a card. Mailing it today!", False),
    ("Jordan", "The library moved our book club to Thursday morning.", False),
]

# --long: two messages long enough to wrap to 4 lines, plus two more, to check
# that both long ones fit and the rest are counted in the footer.
LONG = "--long" in sys.argv
if LONG:
    MESSAGES = [
        ("Alex", "Dinner at our place on Sunday at five. I'll pick you both up at "
                 "four thirty, so be ready by the front door with your coats.", True),
        ("Sam", "The kids finished their school projects and want to show you "
                "everything this weekend. They have been practicing all week!", False),
        ("Jordan", "Book club moved to Thursday.", False),
        ("Alex", "Call me when you get a chance.", False),
    ]
OUT = "board-preview-long.svg" if LONG else "board-preview.svg"

LINE_H = 34
SENDER_H = 24
# Average advance width at the sizes below. The firmware measures glyphs
# properly via getTextBounds; this is only to make the preview representative.
CHAR_W_BODY = 18.6   # ~FreeSans18pt
CHAR_W_SEND = 12.6   # ~FreeSansBold12pt


def wrap(text, max_px, max_lines, char_w):
    max_chars = int(max_px / char_w)
    words, lines, cur = text.split(), [], ""
    for w in words:
        trial = f"{cur} {w}".strip()
        if len(trial) <= max_chars:
            cur = trial
        else:
            lines.append(cur)
            cur = w
            if len(lines) == max_lines:
                break
    if cur and len(lines) < max_lines:
        lines.append(cur)
    return lines[:max_lines]


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


parts = [
    f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
    f'viewBox="0 0 {W} {H}" font-family="DejaVu Sans, Helvetica, Arial, sans-serif">',
    f'<rect width="{W}" height="{H}" fill="{WHITE}"/>',
    # header
    f'<rect x="0" y="0" width="{W}" height="{HEADER_H}" fill="{BLUE}"/>',
    f'<text x="{MARGIN}" y="47" fill="{WHITE}" font-size="34" font-weight="bold">Messages</text>',
    f'<text x="{W - MARGIN}" y="45" fill="{WHITE}" font-size="23" text-anchor="end">Sunday, September 27</text>',
]

y = CONTENT_TOP
hidden = 0
for i, (sender, text, pinned) in enumerate(MESSAGES):
    lines = wrap(text, CONTENT_W - 18, 4, CHAR_W_BODY)
    block_h = SENDER_H + len(lines) * LINE_H + 16
    # Same test as the firmware: only the visible part (to the end of the
    # accent bar, 12 px above the block's bottom) has to fit.
    if y + block_h - 12 > CONTENT_BOTTOM:
        hidden = len(MESSAGES) - i
        break

    accent = RED if pinned else ACCENTS[i % 3]
    parts.append(f'<rect x="{MARGIN}" y="{y}" width="6" height="{block_h - 12}" fill="{accent}"/>')
    label = esc(sender) + ("  *" if pinned else "")
    parts.append(
        f'<text x="{MARGIN + 18}" y="{y + 18}" fill="{accent}" font-size="23" '
        f'font-weight="bold">{label}</text>'
    )

    line_y = y + SENDER_H + 24
    for ln in lines:
        parts.append(
            f'<text x="{MARGIN + 18}" y="{line_y}" fill="{BLACK}" font-size="34">{esc(ln)}</text>'
        )
        line_y += LINE_H
    y += block_h

# footer
fy = H - FOOTER_H
parts.append(f'<line x1="{MARGIN}" y1="{fy}" x2="{W - MARGIN}" y2="{fy}" stroke="{BLACK}" stroke-width="1"/>')
parts.append(f'<text x="{MARGIN}" y="{fy + 24}" fill="{BLACK}" font-size="17">Updated 9:20 AM</text>')
if hidden:
    note = f"+{hidden} more message" + ("" if hidden == 1 else "s")
    parts.append(f'<text x="{W - MARGIN}" y="{fy + 24}" fill="{BLACK}" font-size="17" '
                 f'text-anchor="end">{note}</text>')
parts.append(f'<rect x="0.5" y="0.5" width="{W-1}" height="{H-1}" fill="none" stroke="#888"/>')
parts.append("</svg>")

with open(OUT, "w") as f:
    f.write("\n".join(parts))

print(f"wrote {OUT} ({W}x{H}), {hidden} hidden")
