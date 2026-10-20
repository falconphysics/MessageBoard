/**
 * PhotoPainter Message Board — Gmail side
 *
 * Deploy as a Web App ("Execute as: Me", "Who has access: Anyone with the link").
 * The frame calls the /exec URL with ?key=<SHARED_SECRET> and gets back JSON.
 *
 * How people use it:
 *   - Email the board address. Subject is ignored unless it is a command.
 *   - Subject "pin" (or a subject starting with "!") keeps that message at the top.
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

/** Ignore anything older than this, so a forgotten note doesn't sit there forever. */
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
  const threads = GmailApp.search('in:inbox newer_than:' + MAX_AGE_DAYS + 'd', 0, 40);

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
    candidates.push({ msg: msg, addr: addr, subject: subject, date: date });
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
    const body = cleanBody(c.msg.getPlainBody());
    if (!body) {
      return;
    }
    out.push({
      from: displayName(c.addr),
      text: truncate(body, MAX_CHARS),
      date: c.date.toISOString(),
      pinned: isPinSubject(c.subject),
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
 * "pin", or any subject starting with "!". Re:/Fwd: prefixes are ignored, so
 * replying to a pinned message keeps it pinned.
 */
function isPinSubject(subject) {
  const s = subject.replace(/^((re|fwd?|fw)\s*:\s*)+/i, '').trim();
  return s.toLowerCase() === 'pin' || s.charAt(0) === '!';
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
    Logger.log((i + 1) + '. [' + m.from + ']' + (m.pinned ? ' (pinned)' : '') + ' ' + m.text);
  });
}
