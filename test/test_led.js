// Replays test_led.c's table through ledEval() taken from the phone remote's page.
const fs = require('fs');
const html = fs.readFileSync(process.argv[2], 'utf8');
const src = html.slice(html.indexOf('function ledEval('), html.indexOf('function ledNow('));
const ledEval = new Function(src + '; return ledEval;')();
const CASES = [[2, 7, 3, 0, 255, 160, 0, 10, 30], [2, 16, 255, 0, 0, 200, 255, 50, 0], [3, 3, 5, 7, 255, 0, 0, 0, 0],
               [3, 1, 9, 255, 10, 200, 90, 30, 30], [1, 0, 0, 0, 1, 2, 3, 4, 5]];
const out = [];
CASES.forEach((c, k) => {
  const l = { valid: 1, mode: c[0], a: c[1], b: c[2], c: c[3], to: [c[4], c[5], c[6]], from: [c[7], c[8], 20] };
  for (let t = 0; t <= 3000; t += 7) { const [v] = ledEval(l, t, 23); out.push(`${k} ${t} ${v[0]} ${v[1]} ${v[2]}`); }
});
const want = fs.readFileSync(process.argv[3], 'utf8').trim().split('\n');
let bad = 0;
want.forEach((w, i) => { if (w !== out[i]) { if (bad++ < 5) console.log('differs:', w, '| js:', out[i]); } });
if (bad || want.length !== out.length) { console.log(`LED JS/C mismatch: ${bad} rows`); process.exit(1); }
console.log(`led model: JavaScript matches C (${out.length} samples)`);
