#!/usr/bin/env node
/* bbands_check.js - host check for the dashboard's Bollinger Bands overlay (fw 1.2.1).

   The BB overlay runs in the browser, so it is not covered by the C++ suites. This checks the
   maths that web/index.html actually ships: bbands() is lifted out of the HTML source verbatim
   and compared with an independent, straightforward recomputation (two passes per window,
   population std dev) over:
     - synthetic series (ramp, noise, flat, short, one point) for 9 period/width combinations
     - Yahoo chart fixtures in tests/fixtures (c1d/c5d/c1m), nulls dropped
     - optional live-device /api/chart JSON files given as argv ({"v":[...]})

   Run:  node tests/bbands_check.js [chart.json ...]
   Exit 0 = every value agrees within 1e-6 relative, nulls line up, guards behave. */
'use strict';
const fs = require('fs'), path = require('path');

const HTML = path.join(__dirname, '..', 'web', 'index.html');
const src = fs.readFileSync(HTML, 'utf8');
const m = /function bbands\(v,p,k\)\{[\s\S]*?\nreturn\{md,up,dn,p,k\}\}/.exec(src);
if (!m) { console.error('FAIL: bbands() not found in web/index.html'); process.exit(1); }
let bbands = eval('(' + m[0] + ')');   // the exact shipping implementation (strict mode: eval declarations do not leak)

// Independent reference: no running sums, recomputed from scratch per window.
function ref(v, p, k) {
  const n = v.length, O = { md: Array(n).fill(null), up: Array(n).fill(null), dn: Array(n).fill(null) };
  for (let i = p - 1; i < n; i++) {          // straight two-pass window, no running sums
    const w = v.slice(i - p + 1, i + 1);
    const mu = w.reduce((a, b) => a + b, 0) / p;
    const sd = Math.sqrt(w.reduce((a, b) => a + (b - mu) * (b - mu), 0) / p);
    O.md[i] = mu; O.up[i] = mu + k * sd; O.dn[i] = mu - k * sd;
  }
  return O;
}

let checks = 0, fails = 0;
const bad = (msg) => { console.log('FAIL: ' + msg); fails++; };
function cmp(tag, v, p, k) {
  const A = bbands(v, p, k), B = ref(v, p, k);
  if (v.length < p) {
    checks++;
    if (!A.md.every(x => x === null) || !A.up.every(x => x === null) || !A.dn.every(x => x === null))
      bad(tag + ' p=' + p + ': a period longer than the series must give all nulls');
    return;
  }
  let worst = 0, at = '';
  for (let i = 0; i < v.length; i++) {
    for (const key of ['md', 'up', 'dn']) {
      checks++;
      if ((A[key][i] === null) !== (B[key][i] === null)) { bad(tag + ' ' + key + '[' + i + '] null mismatch p=' + p + ' k=' + k); continue; }
      if (A[key][i] === null) continue;
      const e = Math.abs(A[key][i] - B[key][i]) / Math.max(1, Math.abs(B[key][i]));
      if (e > worst) { worst = e; at = key + '[' + i + '] p=' + p + ' k=' + k; }
    }
  }
  if (worst > 1e-9) bad(tag + ' worst relative error ' + worst.toExponential(2) + ' at ' + at);
  // Bands must enclose the middle line, and the middle must sit inside the price axis sanity.
  for (let i = p - 1; i < v.length; i++) {
    checks++;
    if (!(A.up[i] >= A.md[i] && A.md[i] >= A.dn[i])) bad(tag + ' band order broken at i=' + i + ' p=' + p);
  }
  console.log('  ' + tag.padEnd(26) + ' n=' + String(v.length).padEnd(5) + ' p=' + String(p).padEnd(3) +
    ' k=' + String(k).padEnd(4) + ' worst rel=' + worst.toExponential(2));
}

// ---- synthetic series ----
const series = {
  ramp:  Array.from({ length: 120 }, (_, i) => 100 + i),
  noise: Array.from({ length: 112 }, (_, i) => 7000 + 40 * Math.sin(i / 3) + (i % 7) * 3.5 - 9),
  flat:  Array(40).fill(5),                       // sd must collapse to 0
  short: [10, 11, 12],                            // n < every period below
  one:   [42.5],
};
const noisy = [];
let seed = 12345;
const rnd = () => (seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff;
for (let i = 0; i < 80; i++) noisy.push(6000 + (rnd() - 0.5) * 300);

for (const [name, v] of Object.entries(series))
  for (const p of [10, 20, 50]) for (const k of [1.5, 2, 2.5]) cmp('syn:' + name, v, p, k);
for (const p of [3, 5, 20, 50]) cmp('syn:drift', noisy, p, 2);

// ---- flat series: sd=0 means all three lines coincide ----
{
  const A = bbands(series.flat, 20, 2);
  checks++;
  if (A.up[19] !== A.md[19] || A.md[19] !== A.dn[19] || A.md[19] !== 5) bad('flat series must collapse to the value itself');
}

// ---- Yahoo fixtures ----
for (const f of ['c1d', 'c5d', 'c1m']) {
  const fp = path.join(__dirname, 'fixtures', f + '.json');
  if (!fs.existsSync(fp)) continue;
  const d = JSON.parse(fs.readFileSync(fp, 'utf8'));
  const c = d.chart || d;
  const q = (c.result && c.result[0] && ((c.result[0].indicators && c.result[0].indicators.quote && c.result[0].indicators.quote[0]) ||
    (c.result[0].ind && c.result[0].ind.quote && c.result[0].ind.quote[0]))) || {};
  const v = (q.close || []).filter(x => x != null && isFinite(x));
  if (v.length < 2) { console.log('  fixture ' + f + ': no closes (skipped)'); continue; }
  for (const p of [10, 20]) for (const k of [2]) cmp('fx:' + f, v, p, k);
}

// ---- optional live-device chart JSON ({"v":[...]}) ----
for (const f of process.argv.slice(2)) {
  const d = JSON.parse(fs.readFileSync(f, 'utf8'));
  const v = (d.v || []).filter(x => x != null && isFinite(x));
  if (v.length < 2) { console.log('  ' + f + ': no closes (skipped)'); continue; }
  for (const p of [10, 20, 50]) for (const k of [1.5, 2, 2.5]) cmp('dev:' + path.basename(f), v, p, k);
}

console.log((fails ? 'FAILED ' : 'ok ') + checks + ' checks, ' + fails + ' failed');
process.exit(fails ? 1 : 0);
