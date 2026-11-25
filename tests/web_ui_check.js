#!/usr/bin/env node
/* Dashboard integrity check - the web UI is ONE inline script, so a single typo can make the
   whole page dead: a duplicate top-level `const`/`let` is a parse-time SyntaxError and the browser
   runs nothing at all (this actually happened in fw 1.2.3: the candle settings were named `CK`,
   which the position table already used, and the served dashboard rendered a blank shell while
   every /api endpoint kept working). gz size and grep for markers both pass on such a build, so
   size/grep checks are not enough: the assembled page has to be parsed.

   Checks, in order of what they catch:
     1. every inline <script> parses on its own, and all of them parse as ONE concatenated script
        (that is the case the browser sees, and the only place duplicate-declaration errors show up);
     2. every id the JS looks up with $('id') exists in the markup - a missing element throws a
        TypeError at the same top level and kills everything after it;
     3. the served page (optional URL argument, e.g. http://sp500.local/) is the page we tested:
        same set of ids, same script count, and it parses too.

   Usage:  node tests/web_ui_check.js [served-url-or-file]
   Exits 0 with a check count, 1 on any failure. No dependencies beyond node. */
'use strict';
const fs = require('fs');
const http = require('http');
const path = require('path');
const { execFileSync } = require('child_process');
const zlib = require('zlib');
const os = require('os');

/* The device always serves the dashboard gzipped (curl needs --compressed), so a naive fetch gets
   binary and "0 script blocks" is a false alarm. Decompress when the magic says gzip. */
function decode(buf) {
  if (buf.length > 2 && buf[0] === 0x1f && buf[1] === 0x8b) return zlib.gunzipSync(buf).toString('utf8');
  return buf.toString('utf8');
}

const SRC = path.join(__dirname, '..', 'web', 'index.html');
let checks = 0, fails = [];
function ok(cond, msg) { checks++; if (!cond) fails.push(msg); return !!cond; }

function scriptsOf(html) {
  const out = [];
  const re = /<script([^>]*)>([\s\S]*?)<\/script>/gi;
  let m;
  while ((m = re.exec(html)) !== null) {
    if (/\bsrc\s*=/i.test(m[1])) continue;          // external script: nothing local to check
    out.push(m[2]);
  }
  return out;
}

function parse(label, code) {
  const f = path.join(os.tmpdir(), 'sp500_ui_check_' + label.replace(/\W/g, '_') + '.js');
  fs.writeFileSync(f, code);
  try {
    execFileSync(process.execPath, ['--check', f], { stdio: 'pipe' });
    ok(true, label + ' parses');
  } catch (e) {
    ok(false, label + ' DOES NOT PARSE: ' + String(e.stderr || e).split('\n').slice(0, 6).join(' | '));
  } finally { fs.unlinkSync(f); }
}

const src = fs.readFileSync(SRC, 'utf8');
const js = scriptsOf(src);
ok(js.length >= 1, 'found inline script blocks: ' + js.length);
js.forEach((code, i) => parse('block' + i, code));
parse('concatenated', js.join('\n;\n'));   // the top-level scope the browser actually compiles

/* ids referenced by JS must exist in the markup */
const htmlIds = new Set();
for (const m of src.matchAll(/id\s*=\s*"([^"]+)"/g)) htmlIds.add(m[1]);
const used = new Set();
for (const block of js) for (const m of block.matchAll(/\$\(\s*'([^']+)'\s*\)/g)) used.add(m[1]);
ok(used.size > 30, 'collected ' + used.size + " distinct $('id') lookups (a near-empty set means the pattern broke)");
for (const id of used) ok(htmlIds.has(id), "JS looks up $('" + id + "') but no element with id=\"" + id + '" exists');

/* localStorage keys in use (reported, not asserted: collisions across features are the kind of
   thing a rename like CK->CKS can silently create, so the list is printed at the end) */
const lsKeys = new Set();
for (const block of js) for (const m of block.matchAll(/localStorage\.(?:getItem|setItem)\(\s*'([^']+)'/g)) lsKeys.add(m[1]);

/* every endpoint the dashboard calls must exist in the server's route table: a renamed route means a
   button that silently does nothing. Routes are registered as server.on("/api/x", HTTP_GET|POST, ..) */
const routes = new Set();
const srv = fs.readFileSync(path.join(__dirname, '..', 'src', 'network', 'WebServerManager.cpp'), 'utf8');
for (const m of srv.matchAll(/server\.on\(\s*"(\/api\/[A-Za-z0-9_\/-]*)"/g)) routes.add(m[1]);
ok(routes.size > 15, 'server routes found: ' + routes.size);
const jsPaths = new Set();
for (const block of js) for (const m of block.matchAll(/[`'"]\/api\/[A-Za-z0-9_\/-]+/g)) jsPaths.add(m[0].slice(1));
ok(jsPaths.size > 10, 'dashboard endpoints found: ' + jsPaths.size);
for (const p of jsPaths) ok(routes.has(p.split('?')[0]), 'dashboard calls ' + p + ' but the server never registers it');

/* the candle path must stay guarded: bars are only drawn when the device says it sent them, and a
   null bar must not be drawn as a range. Text-level guard, cheap, and it fails loudly if someone
   rewires the chart and forgets the contract. */
const all = js.join('\n');
ok(/d\.candles&&d\.o&&d\.h&&d\.l/.test(all), 'candle drawing is no longer conditioned on the device reporting candles + o/h/l');
ok(/ck\.h\[i\]!=null/.test(all) && /ck\.l\[i\]!=null/.test(all), 'per-bar null candles are no longer skipped');
ok(/CKS\.on/.test(all) && !/\bCK\.on/.test(all), 'candle settings identifier drift (CK collides with the position-table column list)');

/* the add-position flow must never write a row it has not shown the user: the THB form asks for a
   preview, the dialog renders the close and the session it came from, and only the dialog's own
   button commits the exact quantity/price that was on screen. */
ok(/id="cf"/.test(src) && /\$\('cf-yes'\)\.onclick/.test(all), 'add-position confirm dialog is missing or unwired');
ok(/b\.thb=\+thbv;b\.preview=1/.test(all), 'the THB form no longer asks for a price preview');
ok(/api\('\/api\/positions',\{symbol:a\.sym,date:a\.date,qty:a\.qty,buy:a\.buy\}\)/.test(all), 'the dialog does not commit the price it showed');
ok(/cf-wn/.test(all) && /was not a trading day/.test(all), 'the weekend/holiday warning is gone from the dialog');
ok(!/\$\('a-note'\)/.test(all), 'the old inline add-note is back (it is easy to miss)');

/* served copy, if asked for: same page, and it parses */
const arg = process.argv[2];
if (arg) {
  const get = (u) => new Promise((res, rej) => {
    const r = http.get(u, { timeout: 15000 }, (s) => {
      const b = []; s.on('data', (d) => b.push(d)); s.on('end', () => res(Buffer.concat(b)));
    });
    r.on('error', rej); r.on('timeout', () => { r.destroy(); rej(new Error('timeout')); });
  });
  if (/^https?:/.test(arg)) {
    get(arg).then((buf) => {
      const html = decode(buf);
      ok(html.length > 20000, 'served page length ' + html.length + ' B');
      const sj = scriptsOf(html);
      ok(sj.length === js.length, 'served script blocks ' + sj.length + ' == built ' + js.length);
      parse('served', sj.join('\n;\n'));
      const sids = new Set(); for (const m of html.matchAll(/id\s*=\s*"([^"]+)"/g)) sids.add(m[1]);
      for (const id of used) ok(sids.has(id), 'served page is missing id="' + id + '" the JS needs');
      report('served ' + arg);
    }).catch((e) => { fails.push('could not fetch ' + arg + ': ' + e.message); report(arg); });
  } else {
    const html = decode(fs.readFileSync(arg));
    ok(html.length > 20000, 'served file length ' + html.length + ' B');
    parse('served', scriptsOf(html).join('\n;\n'));
    report('served ' + arg);
  }
} else {
  report(SRC);
}

function report(what) {
  if (fails.length) {
    console.log('FAILED ' + fails.length + ' of ' + checks + ' checks (' + what + ')');
    fails.forEach((f) => console.log('  ! ' + f));
    process.exit(1);
  }
  console.log('OK ' + checks + ' checks, 0 failed (' + what + ')');
  console.log('   ids in markup: ' + htmlIds.size + ', ids used by JS: ' + used.size +
              ', localStorage keys: ' + Array.from(lsKeys).sort().join(', '));
}
