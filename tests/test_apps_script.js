// Run the real apps-script/Code.gs against fake Gmail inboxes.
// Gmail returns threads newest first, which is what the "clear" bug depended on.
// Usage: node test_apps_script.js
const fs = require('fs');
let inbox = [];
function thread(id, from, subject, body, minutesAgo) {
  return { id, archived: false, date: new Date(Date.now() - minutesAgo * 60000),
    getLastMessageDate() { return this.date; },
    getMessages() { const t = this; return [{
      getFrom: () => from, getSubject: () => subject, getPlainBody: () => body,
      getDate: () => t.date }]; },
    moveToArchive() { this.archived = true; } };
}
global.GmailApp = { search: () => inbox.filter(t => !t.archived).sort((a, b) => b.date - a.date) };
global.Utilities = { computeDigest: () => [1, 2, 3], DigestAlgorithm: {}, Charset: {} };
global.ContentService = { createTextOutput: s => ({ setMimeType: () => s }), MimeType: {} };
global.Logger = { log: () => {} };
const CODE = process.argv[2] || require('path').join(__dirname, '..', 'apps-script', 'Code.gs');
eval(fs.readFileSync(CODE, 'utf8').replace(/^const /gm, 'var '));

let fails = 0;
const eq = (name, got, want) => {
  const ok = JSON.stringify(got) === JSON.stringify(want);
  console.log((ok ? '  ok   ' : '  FAIL ') + name + (ok ? '' : `\n       got  ${JSON.stringify(got)}\n       want ${JSON.stringify(want)}`));
  if (!ok) fails++;
};

console.log('=== clear keeps messages sent after it ===');
inbox = [
  thread('old1', 'you@example.com', 'hi', 'Old message one', 300),
  thread('old2', 'you@example.com', 'hi', 'Old message two', 200),
  thread('spam', 'stranger@spam.com', 'hi', 'Not allowed', 150),
  thread('clr',  'you@example.com', 'clear', '', 60),
  thread('new',  'you@example.com', 'hi', 'Sent AFTER the clear', 10),
];
eq('fetch right after clear shows only the newer message', collectMessages().map(m => m.text), ['Sent AFTER the clear']);
eq('archived everything up to the clear', inbox.filter(t => t.archived).map(t => t.id).sort(), ['clr', 'old1', 'old2', 'spam']);
eq('next fetch is the same (no second change, no second flash)', collectMessages().map(m => m.text), ['Sent AFTER the clear']);

console.log('=== clear from a stranger is ignored ===');
inbox = [ thread('a', 'you@example.com', 'hi', 'Keep me', 100),
          thread('c', 'stranger@spam.com', 'clear', '', 5) ];
eq('message survives', collectMessages().map(m => m.text), ['Keep me']);
eq('nothing archived', inbox.filter(t => t.archived).length, 0);

console.log('=== "Re: clear" is not a command ===');
inbox = [ thread('a', 'you@example.com', 'hi', 'Keep me', 100),
          thread('r', 'you@example.com', 'Re: clear', 'ok cleared', 5) ];
eq('nothing archived', inbox.filter(t => t.archived).length, 0);

console.log('=== pins ===');
inbox = [ thread('p', 'you@example.com', 'Re: pin', 'Still pinned', 50),
          thread('b', 'you@example.com', '!Dentist', 'Bang pin', 40),
          thread('f', 'you@example.com', 'FW: Re: pin', 'Forwarded pin', 30),
          thread('n', 'you@example.com', 'Lunch', 'Not pinned', 5) ];
const p = collectMessages();
eq('pinned flags', p.map(m => [m.text, m.pinned]),
   [['Forwarded pin', true], ['Bang pin', true], ['Still pinned', true], ['Not pinned', false]]);

console.log('=== reply headers stripped ===');
const wrapped = 'Sounds good, see you Sunday!\n\nOn Tue, Oct 20, 2026 at 8:39 AM Jordan Smith <jordan.smith@example.com>\nwrote:\n> Dinner at 5?\n';
const long = 'See you then.\n\nOn Tuesday, October 20, 2026 at 8:39:12 AM Eastern Daylight Time, Alexandra Featherstonehaugh <alexandra.featherstonehaugh@example.com> wrote:\n> hi\n';
eq('wrapped Gmail header', cleanBody(wrapped), 'Sounds good, see you Sunday!');
eq('header longer than 80 chars', cleanBody(long), 'See you then.');
eq('ordinary sentence with "on" kept', cleanBody('Meet on the porch at 5.'), 'Meet on the porch at 5.');

console.log(fails ? `\nTESTS FAILED (${fails})` : '\nALL TESTS PASSED');
process.exit(fails ? 1 : 0);
