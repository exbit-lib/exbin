/* Find & replace (plain / case / whole word / regex) with live match highlighting. */
(() => {
'use strict';
const X = self.EXB, $ = X.$, ta = X.ta;
const bar = $('#findbar'), fIn = $('#findIn'), rIn = $('#replIn'), cnt = $('#fCount');
const F = { open: false, cs: false, ww: false, re: false, matches: [], idx: -1, key: '' };
X.find = F;

function buildRegex() {
  const q = fIn.value; if (!q) return null;
  let src = F.re ? q : q.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
  if (F.ww) src = '(?<![\\p{L}\\p{N}_])(?:' + src + ')(?![\\p{L}\\p{N}_])';
  try { const r = new RegExp(src, 'gmu' + (F.cs ? '' : 'i')); fIn.classList.remove('bad'); return r; }
  catch (_) { fIn.classList.add('bad'); return null; }
}
function compute() {
  const code = ta.value, key = code.length + '|' + fIn.value + F.cs + F.ww + F.re;
  F.matches = [];
  const re = F.open ? buildRegex() : null;
  if (re) {
    let m, guard = 0;
    while ((m = re.exec(code)) && guard++ < 5000) {
      if (m[0] === '') { re.lastIndex++; continue; }
      F.matches.push([m.index, m.index + m[0].length]);
    }
  }
  F.key = key;
  syncIdx();
}
function syncIdx() { // current match = first at/after the selection start
  const s = ta.selectionStart; F.idx = -1;
  for (let i = 0; i < F.matches.length; i++) if (F.matches[i][0] >= s) { F.idx = i; break; }
  if (F.idx < 0 && F.matches.length) F.idx = 0;
  const sel = F.matches.findIndex((m) => m[0] === ta.selectionStart && m[1] === ta.selectionEnd);
  if (sel >= 0) F.idx = sel;
  showCount();
}
function showCount() {
  cnt.textContent = !fIn.value ? '' : F.matches.length ? (F.idx + 1) + '/' + F.matches.length : '0/0';
}
X.markers.push((r) => {
  if (!F.open) return;
  F.matches.forEach((m, i) => r.push({ s: m[0], e: m[1], c: i === F.idx && ta.selectionStart === m[0] && ta.selectionEnd === m[1] ? 'fc' : 'fm' }));
});
function refresh() { compute(); X.renderAll(); }

function go(dir) {
  if (!F.matches.length) { compute(); if (!F.matches.length) return; }
  const s = ta.selectionStart, e = ta.selectionEnd, n = F.matches.length;
  let i;
  if (dir > 0) { i = F.matches.findIndex((m) => m[0] >= (e > s ? e : s)); if (i < 0) i = 0; }
  else { i = -1; for (let k = n - 1; k >= 0; k--) if (F.matches[k][0] < s) { i = k; break; } if (i < 0) i = n - 1; }
  F.idx = i;
  const m = F.matches[i];
  X.revealRange(m[0], m[1]);
  showCount();
  if (document.activeElement !== fIn) fIn.focus(); // keep typing in the find box
  X.renderAll();
}
function replaceOne() {
  compute();
  const s = ta.selectionStart, e = ta.selectionEnd;
  const cur = F.matches.find((m) => m[0] === s && m[1] === e);
  if (!cur) return go(1);
  const txt = ta.value.slice(s, e);
  X.replaceRange(s, e, F.re ? txt.replace(new RegExp(buildRegex().source, F.cs ? 'mu' : 'imu'), rIn.value) : rIn.value);
  compute(); go(1);
}
function replaceAll() {
  const re = buildRegex(); if (!re) return;
  compute();
  const n = F.matches.length; if (!n) return X.toast('لا توجد نتائج');
  const code = ta.value;
  const res = code.replace(re, F.re ? rIn.value : () => rIn.value);
  if (res === code) return;
  const keep = ta.scrollTop;
  X.replaceRange(0, code.length, res); ta.scrollTop = keep;
  compute(); X.renderAll();
  X.toast('استُبدل ' + n + ' موضع');
}

function open(replace, seed) {
  F.open = true; bar.hidden = false; $('#replRow').hidden = !replace;
  const s = ta.selectionStart, e = ta.selectionEnd, sel = ta.value.slice(s, e);
  if (seed !== false && sel && !sel.includes('\n') && sel.length < 200) fIn.value = sel;
  fIn.focus(); fIn.select();
  refresh();
}
function close() {
  if (!F.open) return;
  F.open = false; bar.hidden = true; F.matches = [];
  X.renderAll(); ta.focus();
}
X.openFind = open; X.closeFind = close;

fIn.addEventListener('input', refresh);
fIn.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { e.preventDefault(); go(e.shiftKey ? -1 : 1); }
  else if (e.key === 'Escape') { e.preventDefault(); close(); }
});
rIn.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { e.preventDefault(); if (e.ctrlKey || e.metaKey) replaceAll(); else replaceOne(); }
  else if (e.key === 'Escape') { e.preventDefault(); close(); }
});
for (const [id, k] of [['#fCase', 'cs'], ['#fWord', 'ww'], ['#fRe', 're']]) {
  $(id).addEventListener('click', () => { F[k] = !F[k]; $(id).classList.toggle('on', F[k]); refresh(); });
}
$('#fNext').addEventListener('click', () => go(1));
$('#fPrev').addEventListener('click', () => go(-1));
$('#fClose').addEventListener('click', close);
$('#fRepl').addEventListener('click', replaceOne);
$('#fReplAll').addEventListener('click', replaceAll);
$('#btnFind').addEventListener('click', () => (F.open ? close() : open(false)));
X.hooks.input.push(() => { if (F.open) compute(); });
X.hooks.file.push(() => { if (F.open) compute(); });
X.hooks.select.push(() => { if (F.open) showCount(); });
})();
