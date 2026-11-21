// Runs tests/lisp_cases.txt against the JavaScript copy of the language (src/web/cairn.js).
// Usage: node tests/lisp_js_test.js [path-to-cairn.js]
const fs = require('fs');
const file = process.argv[2] || 'src/web/cairn.js';
new Function(fs.readFileSync(file, 'utf8')).call(globalThis);
const { Lisp } = globalThis.Cairn;

function runCapture(src) {
  let out = '';
  const props = { 'box/text': { isStr: true, n: 0, s: 'hello' }, 'box/x': { isStr: false, n: 10, s: '' } };
  const host = {
    gotoPage: t => { out += '[goto ' + t + ']\n'; return true; },
    gotoRelative: w => { out += '[goto ' + w + ']\n'; return true; },
    goBack: () => { out += '[back]\n'; return true; },
    say: m => { out += m + '\n'; },
    open: t => { out += '[open ' + t + ']\n'; return true; },
    play: n => { out += '[play ' + n + ']\n'; return true; },
    stop: () => { out += '[stop]\n'; },
    pageTitle: () => 'Test page',
    getProp: (n, p) => props[n + '/' + p] || null,
    setProp: (n, p, v) => { if (n !== 'box') return false; props[n + '/' + p] = v; return true; },
  };
  const l = new Lisp();
  let r = l.run(src, host);
  for (let t = 10; r.ok && t <= 5000 && l.hasTimers(); t += 10) r = l.tick(t, host);
  if (!r.ok) out += 'ERROR: ' + r.error + '\n';
  return out;
}

const text = fs.readFileSync('tests/lisp_cases.txt', 'utf8').replace(/\n$/, '').split('\n');
let name = '', prog = '', want = '', mode = 0, fails = 0, cases = 0;
function finish() {
  if (!name) return;
  cases++;
  const got = runCapture(prog).replace(/\n$/, ''), w = want.replace(/\n$/, '');
  if (got !== w) { fails++; console.log(`FAIL ${name}\n  want: ${JSON.stringify(w)}\n  got:  ${JSON.stringify(got)}`); }
}
for (const line of text) {
  if (line.startsWith('### ')) { finish(); name = line.slice(4); prog = ''; want = ''; mode = 1; }
  else if (line === '---' && mode === 1) mode = 2;
  else if (mode === 1) prog += line + '\n';
  else if (mode === 2) want += line + '\n';
}
finish();
console.log(`${cases} cases, ${fails} failed`);
process.exit(fails ? 1 : 0);
