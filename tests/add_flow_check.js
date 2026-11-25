/* Drives the real add-position code out of web/index.html in a fake DOM.
   The point: a THB amount must never become a row by itself. The lookup runs first, the confirm
   dialog opens with the close and the session it came from, and the row is only written by the
   dialog's own button, with the exact quantity and price that were on screen. */
'use strict';
const fs = require('fs'), path = require('path');
/* index.html by default; pass a saved copy of a page the device is serving to test that build */
const SRC = process.argv[2] || path.join(__dirname, '..', 'web', 'index.html');
const zlib = require('zlib');
const rawf = fs.readFileSync(SRC);
const src = (rawf.length > 2 && rawf[0] === 0x1f && rawf[1] === 0x8b) ? zlib.gunzipSync(rawf).toString('utf8') : rawf.toString('utf8');

/* the helpers the block needs are taken from the page itself, not reimplemented here */
function span(from, to) {
  const i = src.indexOf(from);
  if (i < 0) throw new Error('page no longer contains ' + from);
  const j = src.indexOf(to, i);
  if (j < 0) throw new Error('page no longer contains ' + to);
  const e = src.indexOf('\n', j);
  return src.slice(i, e < 0 ? src.length : e);
}
const helpers = span('const todayISO=', 'const thb=(x,s,d)');

const a = src.indexOf('let AV=null,PE=');
const b = src.indexOf('/* ---- sales:');
if (a < 0 || b < 0 || b < a) throw new Error('add-position block not found in web/index.html');
const block = src.slice(a, b);

let checks = 0, fails = [];
const ok = (c, m) => { checks++; if (!c) fails.push(m); };

/* ---- minimal DOM ---- */
function el() {
  const s = new Set();
  return { value: '', textContent: '', innerHTML: '', className: '', disabled: false, hidden: false,
           onclick: null, onsubmit: null, oninput: null,
           classList: { add: (c) => s.add(c), remove: (c) => s.delete(c), contains: (c) => s.has(c) } };
}
const els = {};
const $ = (id) => (els[id] || (els[id] = el()));
const posts = [], toasts = [];
let loads = 0, job = null;
const api = async (p, body) => {
  posts.push([p, body || null]);
  if (p === '/api/positions/job') return job;
  if (p === '/api/positions' && body && body.qty) return { ok: true, state: 'ok' };
  return { ok: true, state: 'pending' };
};
const sl = () => Promise.resolve();
const toast = (t) => toasts.push(t);
const ldPos = async () => { loads++; };
const document = { addEventListener() {} };

/* ---- run the page's add-position code ---- */
const run = new Function('$', 'api', 'sl', 'toast', 'ldPos', 'document', 'isFinite',
  helpers + '\n' + block + '\nreturn {af: $(\'af\'), yes: $(\'cf-yes\'), no: $(\'cf-no\')};');
const ui = run($, api, sl, toast, ldPos, document, isFinite);

async function fill(date, thb) {
  posts.length = 0; loads = 0;
  $('a-sym').value = '^GSPC'; $('a-date').value = date; $('a-thb').value = thb;
  $('a-qty').value = ''; $('a-buy').value = '';
  await ui.af.onsubmit({ preventDefault() {} });
}

(async () => {
  /* Sat 21/11/2026: Yahoo has no bar for it, the device answers with Fri 20/11/2026's close */
  job = { state: 'ok', buy: 6910.39, qty: 1.28913, priceDate: '2026-11-20', exact: false };
  await fill('21/11/2026', '10000');
  ok(posts.length === 2 && posts[0][0] === '/api/positions' && posts[0][1].thb === 10000 &&
     posts[0][1].preview === 1 && posts[0][1].date === '2026-11-21',
     'first press must POST thb+preview, got ' + JSON.stringify(posts[0][1]));
  ok(!posts.some((p) => p[1] && p[1].qty), 'a row was written without confirming');
  ok($('cf').classList.contains('on'), 'the confirm dialog did not open');
  ok($('cf-sub').textContent === '^GSPC on 21/11/2026', 'dialog subtitle: ' + $('cf-sub').textContent);
  ok($('cf-buy').textContent === '6,910.39 pts', 'dialog close: ' + $('cf-buy').textContent);
  ok($('cf-day').textContent === 'Fri 20/11/2026', 'dialog session: ' + $('cf-day').textContent);
  ok($('cf-qty').textContent === '1.289130', 'dialog quantity: ' + $('cf-qty').textContent);
  ok(!$('cf-wn').hidden && /21\/11\/2026 was not a trading day/.test($('cf-wn').innerHTML),
     'the not-a-trading-day warning is missing');
  ok(/THB/.test($('cf-thb').textContent) && /THB/.test($('cf-cost').textContent), 'money rows empty');

  /* confirm: the row goes in with exactly the numbers that were shown */
  await ui.yes.onclick();
  const w = posts.find((p) => p[1] && p[1].qty);
  ok(!!w && w[1].qty === 1.28913 && w[1].buy === 6910.39 && w[1].date === '2026-11-21' &&
     w[1].symbol === '^GSPC', 'the write did not carry the shown price: ' + JSON.stringify(w && w[1]));
  ok(!($('cf').classList.contains('on')), 'dialog stayed open after confirming');
  ok(loads === 1, 'positions were not reloaded after the write');

  /* an exact weekday close: no warning, dialog still required */
  job = { state: 'ok', buy: 7000, qty: 1.428571, priceDate: '2026-11-20', exact: true };
  await fill('20/11/2026', '10000');
  ok($('cf').classList.contains('on') && $('cf-wn').hidden, 'exact close: dialog on, warning hidden');
  ok($('cf-day').textContent === 'Fri 20/11/2026', 'exact close session: ' + $('cf-day').textContent);

  /* cancel: nothing is written, and the form is usable again */
  posts.length = 0;
  await ui.no.onclick();
  ok(!($('cf').classList.contains('on')), 'dialog did not close on cancel');
  ok(!posts.length, 'cancel still wrote something: ' + JSON.stringify(posts));
  ok(/nothing was saved/i.test($('a-msg').textContent), 'cancel message: ' + $('a-msg').textContent);

  /* a rejected lookup must not open a dialog over a row nobody confirmed */
  job = { state: 'error', error: 'low memory' };
  posts.length = 0; loads = 0;
  await fill('21/11/2026', '10000');
  ok(!$('cf').classList.contains('on') && /low memory/.test($('a-msg').textContent),
     'lookup error was not reported inline: ' + $('a-msg').textContent);
  ok(!posts.some((p) => p[1] && p[1].qty), 'a write happened after a failed lookup');

  /* manual quantity + price: no lookup, no dialog (the user typed both numbers) */
  posts.length = 0; loads = 0;
  $('a-thb').value = ''; $('a-qty').value = '2'; $('a-buy').value = '7100';
  await ui.af.onsubmit({ preventDefault() {} });
  ok(posts.length === 1 && posts[0][1].qty === 2 && !$('cf').classList.contains('on'),
     'manual entry should post straight away: ' + JSON.stringify(posts));

  if (fails.length) { console.log('FAILED ' + fails.length + ' of ' + checks); fails.forEach((f) => console.log('  ! ' + f)); process.exit(1); }
  console.log('OK ' + checks + ' checks, 0 failed (add-position flow, ' + path.basename(SRC) + (process.argv[2] ? ' [served]' : '') + ')');
})();
