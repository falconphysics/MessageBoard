/**
 * PhotoPainter Message Board — Gmail side
 *
 * Deploy as a Web App ("Execute as: Me", "Who has access: Anyone with the link").
 * The frame calls the /exec URL with ?key=<SHARED_SECRET> and gets back JSON.
 *
 * How people use it:
 *   - Email the board address. Subject is ignored unless it is a command.
 *   - Subject "pin" (or a subject starting with "!") keeps that message at the top.
 *   - Subject "pin" plus a date ("Pin 10/25", "Pin Oct 25", "Pin Saturday")
 *     keeps it at the top through that day; it disappears the day after.
 *   - Subject "clear" archives everything currently on the board.
 *   - Archiving a message in the board's Gmail inbox removes it from the frame.
 */

// ---------------------------------------------------------------------------
// CONFIG — edit these three things.
// ---------------------------------------------------------------------------

/** Must match SHARED_SECRET in the firmware's config.h. Make up a long random string. */
const SHARED_SECRET = 'CHANGE-ME-to-a-long-random-string';

/**
 * Only mail from these addresses reaches the board. This is the spam filter —
 * do not leave it empty. Lowercase. A leading '@' allows a whole domain.
 */
const ALLOWED_SENDERS = [
  'you@example.com',
  'sibling@example.com',
  // '@example.org',                 // whole-domain example: allows anyone at that domain
];

/**
 * How many messages to send to the frame (up to 8). About 4 fit on screen at
 * this type size; any that don't are counted in the footer as "+N more".
 */
const MAX_MESSAGES = 4;

/**
 * Ignore anything older than this, so a forgotten note doesn't sit there
 * forever. Messages pinned to a date ("Pin 12/25") are the exception: they
 * stay until their date has passed, however far ahead it is.
 */
const MAX_AGE_DAYS = 21;

/** Hard cap on a single message so one long email can't push the rest off. */
const MAX_CHARS = 180;

// ---------------------------------------------------------------------------

function doGet(e) {
  const params = (e && e.parameter) || {};

  if (params.key !== SHARED_SECRET) {
    return json({ error: 'unauthorized' });
  }

  try {
    const messages = collectMessages();
    return json({
      generated: new Date().toISOString(),
      hash: hashOf(messages),
      count: messages.length,
      messages: messages,
    });
  } catch (err) {
    // Never throw at the frame — it should show something rather than nothing.
    return json({ error: String(err), messages: [], hash: 'error', count: 0 });
  }
}

/**
 * Pull the current board contents out of the inbox.
 */
function collectMessages() {
  const today = todayParts();
  const todayKey = dayKey(today);

  // Recent mail, plus older mail with "pin" in the subject: a message pinned
  // to a date a month away has to outlive MAX_AGE_DAYS. Both searches can
  // return the same thread, so de-duplicate.
  const seen = {};
  const threads = []
    .concat(GmailApp.search('in:inbox newer_than:' + MAX_AGE_DAYS + 'd', 0, 40))
    .concat(GmailApp.search('in:inbox older_than:' + MAX_AGE_DAYS + 'd subject:pin', 0, 40))
    .filter(function (t) {
      if (seen[t.getId()]) return false;
      seen[t.getId()] = true;
      return true;
    });
  const oldest = Date.now() - MAX_AGE_DAYS * 24 * 60 * 60 * 1000;

  // First pass: gather the allowed messages and find the most recent "clear".
  // Both passes are needed because Gmail returns threads newest first: a
  // single pass would wipe messages sent AFTER the clear, and then re-add the
  // older ones it had just archived.
  const candidates = [];
  let clearedAt = null;

  threads.forEach(function (thread) {
    // Use the newest message in the thread, so a reply updates rather than duplicates.
    const msgs = thread.getMessages();
    const msg = msgs[msgs.length - 1];

    const addr = parseAddress(msg.getFrom());
    if (!isAllowed(addr.email)) {
      return;
    }

    const subject = (msg.getSubject() || '').trim();
    const date = msg.getDate();
    if (isClearCommand(subject)) {
      if (!clearedAt || date > clearedAt) clearedAt = date;
      return;
    }
    candidates.push({ thread: thread, msg: msg, addr: addr, subject: subject, date: date });
  });

  // "clear" wipes everything up to and including the clear email itself.
  // Anything sent after it stays.
  if (clearedAt) {
    GmailApp.search('in:inbox', 0, 100).forEach(function (t) {
      if (t.getLastMessageDate() <= clearedAt) t.moveToArchive();
    });
  }

  const out = [];
  candidates.forEach(function (c) {
    if (clearedAt && c.date <= clearedAt) {
      return;
    }

    const pin = pinInfo(c.subject, today);
    if (pin.until) {
      // Pinned to a date: shown through that whole day, gone the day after.
      // Archive it once it's over so it stops turning up in the search.
      if (dayKey(pin.until) < todayKey) {
        c.thread.moveToArchive();
        return;
      }
    } else if (c.date.getTime() < oldest) {
      return;   // only dated pins are allowed past MAX_AGE_DAYS
    }

    const body = cleanBody(c.msg.getPlainBody());
    if (!body) {
      return;
    }
    out.push({
      from: displayName(c.addr),
      text: truncate(body, MAX_CHARS),
      date: c.date.toISOString(),
      pinned: pin.pinned,
      // Not used by the frame; shown by testBoard() so you can check a date
      // was understood. Not part of the hash.
      until: pin.until ? isoDay(pin.until) : null,
    });
  });

  // Pinned first, then newest first.
  out.sort(function (a, b) {
    if (a.pinned !== b.pinned) return a.pinned ? -1 : 1;
    return b.date.localeCompare(a.date);
  });

  return out.slice(0, MAX_MESSAGES);
}

/** "clear" or "clear board". A reply ("Re: clear") is deliberately not a command. */
function isClearCommand(subject) {
  const s = subject.toLowerCase();
  return s === 'clear' || s === 'clear board';
}

/**
 * Read a subject's pin instructions. Returns { pinned, until }, where until is
 * { y, m, d } for a dated pin and null otherwise.
 *
 *   "pin"                     pinned, no end date
 *   "! anything"              pinned, no end date
 *   "Pin 10/25"               pinned through Oct 25
 *   "Pin 10/25 Dentist"       the same; words after the date are ignored
 *   "Pin Oct 25", "Pin 25 October", "Pin 2026-10-25", "Pin 10/25/26"
 *   "Pin Saturday"            through the next Saturday (today, if it is Saturday)
 *   "Pin today", "Pin tomorrow"
 *   "Pin <not a date>"        pinned, no end date
 *
 * Re:/Fwd: prefixes are ignored, so replying to a pinned message keeps it
 * pinned. Numeric dates are month/day, as written in the US.
 */
function pinInfo(subject, today) {
  const s = subject.replace(/^((re|fwd?|fw)\s*:\s*)+/i, '').trim();
  if (s.charAt(0) === '!') return { pinned: true, until: null };
  const m = s.match(/^pin\b\s*:?\s*([\s\S]*)$/i);
  if (!m) return { pinned: false, until: null };
  return { pinned: true, until: parsePinDate(m[1], today) };
}

const MONTH_NAMES = ['january', 'february', 'march', 'april', 'may', 'june', 'july',
                     'august', 'september', 'october', 'november', 'december'];
const DAY_NAMES = ['sunday', 'monday', 'tuesday', 'wednesday', 'thursday', 'friday', 'saturday'];

/**
 * Parse the date at the start of `text`. Returns { y, m, d } or null.
 * Without a year, picks whichever of last year, this year or next year is
 * closest to today: "Pin 1/5" sent in December means next January, and
 * "Pin 10/20" sent on Oct 21 means yesterday (so it has already expired).
 */
function parsePinDate(text, today) {
  let t = String(text || '').trim().toLowerCase()
    .replace(/^(on|until|till|thru|through)\s+/, '');

  if (/^today\b/.test(t)) return today;
  if (/^tomorrow\b/.test(t)) return addDays(today, 1);

  // A leading weekday is either the whole date ("Pin Saturday") or decoration
  // in front of one ("Pin Sat 10/25").
  let weekday = -1;
  const wd = t.match(/^([a-z]{3,})\.?[,:]?(\s+|$)/);
  if (wd) {
    weekday = matchName(wd[1], DAY_NAMES);
    if (weekday >= 0) t = t.slice(wd[0].length);
  }

  let y = null, mo = 0, d = 0, mm;
  if ((mm = t.match(/^(\d{4})-(\d{1,2})-(\d{1,2})\b/))) {                 // 2026-10-25
    y = +mm[1]; mo = +mm[2]; d = +mm[3];
  } else if ((mm = t.match(/^(\d{1,2})[\/.\-](\d{1,2})(?:[\/.\-](\d{4}|\d{2}))?\b/))) {
    mo = +mm[1]; d = +mm[2];                                                // 10/25, 10/25/26
    if (mm[3]) y = mm[3].length === 2 ? 2000 + +mm[3] : +mm[3];
  } else if ((mm = t.match(/^([a-z]{3,})\.?\s+(\d{1,2})(?:st|nd|rd|th)?\b(?:,?\s+(\d{4})\b)?/))) {
    mo = matchName(mm[1], MONTH_NAMES) + 1; d = +mm[2];                     // Oct 25, 2026
    if (mm[3]) y = +mm[3];
  } else if ((mm = t.match(/^(\d{1,2})(?:st|nd|rd|th)?\s+([a-z]{3,})\b\.?(?:,?\s+(\d{4})\b)?/))) {
    d = +mm[1]; mo = matchName(mm[2], MONTH_NAMES) + 1;                     // 25 October
    if (mm[3]) y = +mm[3];
  } else if (weekday >= 0) {
    const ahead = (weekday - dayOfWeek(today) + 7) % 7;                     // Saturday
    return addDays(today, ahead);
  } else {
    return null;
  }

  if (mo < 1 || mo > 12 || d < 1 || d > 31) return null;
  if (y !== null) return isRealDate(y, mo, d) ? { y: y, m: mo, d: d } : null;

  let best = null;
  [today.y - 1, today.y, today.y + 1].forEach(function (cy) {
    if (!isRealDate(cy, mo, d)) return;
    const gap = Math.abs(dayNumber({ y: cy, m: mo, d: d }) - dayNumber(today));
    if (!best || gap < best.gap) best = { gap: gap, date: { y: cy, m: mo, d: d } };
  });
  return best ? best.date : null;
}

/** Index of the name `word` abbreviates ("oct", "sept", "sat"), or -1. */
function matchName(word, names) {
  for (let i = 0; i < names.length; i++) {
    if (names[i].indexOf(word) === 0) return i;
  }
  return -1;
}

/** Today's date in the script's time zone (Project Settings > Time zone). */
function todayParts() {
  const p = Utilities.formatDate(new Date(), Session.getScriptTimeZone(), 'yyyy-M-d').split('-');
  return { y: +p[0], m: +p[1], d: +p[2] };
}

function isRealDate(y, m, d) {
  const dt = new Date(Date.UTC(y, m - 1, d));
  return dt.getUTCFullYear() === y && dt.getUTCMonth() === m - 1 && dt.getUTCDate() === d;
}
function dayNumber(p) { return Math.round(Date.UTC(p.y, p.m - 1, p.d) / 86400000); }
function dayOfWeek(p) { return new Date(Date.UTC(p.y, p.m - 1, p.d)).getUTCDay(); }
function addDays(p, n) {
  const dt = new Date(Date.UTC(p.y, p.m - 1, p.d + n));
  return { y: dt.getUTCFullYear(), m: dt.getUTCMonth() + 1, d: dt.getUTCDate() };
}
function dayKey(p) { return p.y * 10000 + p.m * 100 + p.d; }
function isoDay(p) {
  return p.y + '-' + (p.m < 10 ? '0' : '') + p.m + '-' + (p.d < 10 ? '0' : '') + p.d;
}

/**
 * Strip the parts of an email nobody wants on a wall display:
 * quoted replies, forwarded headers, signatures, and phone footers.
 */
function cleanBody(raw) {
  if (!raw) return '';
  let text = raw.replace(/\r\n/g, '\n');

  const cutPoints = [
    // Gmail reply header. It runs long with a full name and address, and is
    // often wrapped onto a second line, so allow both.
    /\n\s*On [\s\S]{0,300}?wrote:\s*(\n|$)/i,
    /\n\s*-{2,}\s*Original Message/i,
    /\n\s*_{5,}/,                          // Outlook divider
    /\n\s*From:\s.+\nSent:\s/i,
    /\n\s*--\s*\n/,                        // signature delimiter
    /\n\s*Sent from my \w+/i,
    /\n\s*Get Outlook for \w+/i,
  ];
  cutPoints.forEach(function (re) {
    const m = text.match(re);
    if (m && m.index !== undefined) {
      text = text.slice(0, m.index);
    }
  });

  // Drop any remaining quoted lines and collapse whitespace.
  text = text
    .split('\n')
    .filter(function (line) { return line.trim().charAt(0) !== '>'; })
    .join(' ')
    .replace(/\s+/g, ' ')
    .trim();

  return text;
}

function parseAddress(from) {
  // "Jane Doe <jane@example.com>" or bare "jane@example.com"
  const angled = from.match(/^\s*"?([^"<]*?)"?\s*<([^>]+)>\s*$/);
  if (angled) {
    return { name: angled[1].trim(), email: angled[2].trim().toLowerCase() };
  }
  return { name: '', email: from.trim().toLowerCase() };
}

function isAllowed(email) {
  return ALLOWED_SENDERS.some(function (entry) {
    const rule = entry.trim().toLowerCase();
    if (!rule) return false;
    if (rule.charAt(0) === '@') return email.endsWith(rule);
    return email === rule;
  });
}

function displayName(addr) {
  if (addr.name) {
    // First name is plenty on a wall display.
    return addr.name.split(/\s+/)[0];
  }
  return addr.email.split('@')[0];
}

function truncate(text, limit) {
  if (text.length <= limit) return text;
  const cut = text.slice(0, limit);
  const lastSpace = cut.lastIndexOf(' ');
  return (lastSpace > limit * 0.6 ? cut.slice(0, lastSpace) : cut).trim() + '…';
}

/**
 * Content fingerprint. The frame skips its 25-second refresh when this
 * is unchanged, which is the whole point of doing it server-side.
 */
function hashOf(messages) {
  const payload = messages
    .map(function (m) { return m.from + '\u0001' + m.text + '\u0001' + m.pinned; })
    .join('\u0002');
  const digest = Utilities.computeDigest(Utilities.DigestAlgorithm.MD5, payload, Utilities.Charset.UTF_8);
  return digest
    .map(function (b) { return ((b & 0xff) + 0x100).toString(16).slice(1); })
    .join('');
}

function json(obj) {
  return ContentService
    .createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}

/**
 * Run this from the Apps Script editor to check your filters and formatting
 * before the frame ever sees it. Output goes to the execution log.
 */
function testBoard() {
  const messages = collectMessages();
  Logger.log('hash: ' + hashOf(messages));
  Logger.log('count: ' + messages.length);
  messages.forEach(function (m, i) {
    const pin = m.until ? ' (pinned through ' + m.until + ')' : m.pinned ? ' (pinned)' : '';
    Logger.log((i + 1) + '. [' + m.from + ']' + pin + ' ' + m.text);
  });
}
