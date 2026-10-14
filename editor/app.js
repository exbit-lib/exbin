/* EXBIN editor core — state, rendering, editing, runner.  Dependency-free, offline-first.
   Other modules (find.js, complete.js, ui.js) plug in through the global `EXB` object. */
(() => {
'use strict';
const L = self.EXB_LANG, DATA = self.EXB_DATA;
const $ = (s, r) => (r || document).querySelector(s);
const X = self.EXB = { L, $, hooks: { input: [], select: [], render: [], file: [], run: [] }, markers: [], cmds: [] };
const safe = (f, d) => { try { return f(); } catch (_) { return d; } };
const esc = L.esc;

/* --------------------------------------------------------------- state -- */
const KEY = 'exbin.editor.v1';
const DEFAULTS = { tab: 2, autoClose: true, live: true, curLine: true, occ: true, suggest: true, sigHelp: true };
let st = safe(() => JSON.parse(localStorage.getItem(KEY)), null);
if (!st || !Array.isArray(st.files) || !st.files.length) {
  st = { files: [{ id: 1, name: 'main.exb', code: DATA.examples[0].code }], active: 1, theme: null, fs: 15, nextId: 2 };
}
st.fs = st.fs || 15; st.set = Object.assign({}, DEFAULTS, st.set || {}); st.trash = st.trash || []; st.stdin = st.stdin || '';
for (const f of st.files) if (typeof f.code !== 'string') f.code = '';
X.st = st;
const ind = () => ' '.repeat(st.set.tab);
X.ind = ind;
let saveTimer = 0;
const savedEl = $('#saved');
function persist() {
  savedEl.textContent = 'يحفظ…';
  clearTimeout(saveTimer);
  saveTimer = setTimeout(() => { safe(() => localStorage.setItem(KEY, JSON.stringify(st))); savedEl.textContent = 'محفوظ'; }, 300);
}
const cur = () => st.files.find((f) => f.id === st.active) || st.files[0];
X.persist = persist; X.cur = cur;

/* ------------------------------------------------------------ elements -- */
const ta = $('#ta'), hl = $('#hl'), mk = $('#mk'), gutter = $('#gutter'), out = $('#out'), meas = $('#meas'), curLineEl = $('#curLine');
X.ta = ta; X.out = out;

function toast(msg) {
  const t = $('#toast'); t.textContent = msg; t.hidden = false;
  clearTimeout(toast.t); toast.t = setTimeout(() => { t.hidden = true; }, 2400);
}
X.toast = toast;

/* --------------------------------------------------------- parse cache -- */
let P = { code: null };
function parse() {
  const code = ta.value;
  if (P.code === code) return P;
  const toks = L.tokenize(code);
  const syms = L.symbols(code, toks);
  const br = L.matchBrackets(toks);
  P = { code, toks, syms, pairs: br.pairs, bad: br.bad, starts: L.lineStarts(code),
    userFns: new Set(syms.filter((s) => s.kind === 'fn').map((s) => s.name)) };
  return P;
}
X.parse = parse;
const tokAt = (pos) => { // token containing pos (binary search)
  const t = parse().toks; let lo = 0, hi = t.length - 1;
  while (lo <= hi) { const m = (lo + hi) >> 1; if (t[m].e <= pos) lo = m + 1; else if (t[m].s > pos) hi = m - 1; else return t[m]; }
  return null;
};
X.tokAt = tokAt;
/* is `pos` inside a string or comment (where auto-close / completion must stay quiet)? */
X.inLiteral = (pos) => {
  const k = tokAt(pos - 1); if (!k || (k.t !== 'c' && k.t !== 's') || pos <= k.s) return false;
  const txt = ta.value.slice(k.s, k.e);
  if (k.t === 's') return pos < k.e || !(txt.length >= 2 && txt.endsWith(txt[0]));
  if (txt.startsWith('//')) return true;
  return pos < k.e || !txt.endsWith('*/');
};

/* ------------------------------------------------------------- diagnostics -- */
const diag = X.diag = { live: {}, run: [] }; // live: fileId -> [{line,msg}]; run: [{file,line,msg}]
function fileDiags(f) { // all diagnostics for a file as [{line,msg,src}]
  const a = (diag.live[f.id] || []).map((d) => ({ ...d, src: 'check' }));
  for (const d of diag.run) if (d.file === f.name) a.push({ ...d, src: 'run' });
  return a;
}
X.fileDiags = fileDiags;
function lineDiagMap() { const m = new Map(); for (const d of fileDiags(cur())) if (!m.has(d.line)) m.set(d.line, d); return m; }
X.lineDiagMap = lineDiagMap;

/* -------------------------------------------------------------- render -- */
let lastGutterKey = '';
const caretLine = () => { const p = parse(); return L.lineOf(p.starts, ta.selectionStart); };
function renderGutter(force) {
  const p = parse(), n = p.starts.length, cl = caretLine(), dm = lineDiagMap();
  const key = n + '|' + cl + '|' + [...dm.keys()].join(',');
  if (key === lastGutterKey && !force) return;
  lastGutterKey = key;
  let g = '';
  for (let i = 1; i <= n; i++) g += dm.has(i) ? '<span class="err">' + i + '</span>\n' : i === cl ? '<span class="cur">' + i + '</span>\n' : i + '\n';
  gutter.innerHTML = g;
  gutter.scrollTop = ta.scrollTop;
}
function renderCode() {
  const p = parse();
  hl.innerHTML = L.highlight(p.code, p.toks, p.userFns);
  renderGutter(true);
}
/* Marker layer: background-only decorations behind the text. */
function renderMarks() {
  const p = parse();
  if (p.code.length > 400000) { mk.textContent = ''; return; }
  const ranges = [];
  for (const fn of X.markers) fn(ranges, p);
  if (!ranges.length) { if (mk.firstChild) mk.textContent = ''; }
  else {
    const ev = [];
    for (const r of ranges) if (r.e > r.s) { ev.push([r.s, 1, r.c], [r.e, -1, r.c]); }
    ev.sort((a, b) => a[0] - b[0] || a[1] - b[1]);
    const active = {}; let html = '', last = 0;
    const flush = (to) => {
      if (to <= last) return;
      const cls = Object.keys(active).filter((k) => active[k] > 0).join(' ');
      const txt = esc(p.code.slice(last, to));
      html += cls ? '<span class="' + cls + '">' + txt + '</span>' : txt;
      last = to;
    };
    for (const [pos, d, c] of ev) { flush(pos); active[c] = (active[c] || 0) + d; }
    flush(p.code.length);
    mk.innerHTML = html + '\n ';
  }
  syncScroll();
}
// built-in markers -----------------------------------------------------------
X.markers.push((r, p) => { // diagnostics squiggles
  const dm = lineDiagMap();
  for (const [line] of dm) {
    const s = p.starts[line - 1]; if (s === undefined) continue;
    const e = line < p.starts.length ? p.starts[line] - 1 : p.code.length;
    const txt = p.code.slice(s, e), lead = txt.length - txt.trimStart().length;
    r.push({ s: s + lead, e: Math.max(s + lead + 1, s + txt.trimEnd().length), c: 'dg' });
  }
});
X.markers.push((r, p) => { // matching brackets
  const pos = ta.selectionStart; if (pos !== ta.selectionEnd) return;
  for (const q of [pos - 1, pos]) {
    if (p.pairs.has(q)) { r.push({ s: q, e: q + 1, c: 'br' }, { s: p.pairs.get(q), e: p.pairs.get(q) + 1, c: 'br' }); break; }
  }
});
X.markers.push((r, p) => { // other occurrences of the word under the caret
  if (!st.set.occ) return;
  const s = ta.selectionStart, e = ta.selectionEnd;
  let w = null;
  if (s === e) { const k = tokAt(s) || tokAt(s - 1); if (k && k.t === 'w' && s >= k.s && s <= k.e) w = k.v; }
  else { const k = tokAt(s); if (k && k.t === 'w' && k.s === s && k.e === e) w = k.v; }
  if (!w) return;
  const hits = p.toks.filter((k) => k.t === 'w' && k.v === w);
  if (hits.length < 2 || hits.length > 300) return;
  for (const k of hits) r.push({ s: k.s, e: k.e, c: 'oc' });
});

function lh() { return parseFloat(getComputedStyle(ta).lineHeight) || 24; }
X.lh = lh;
function syncScroll() {
  hl.scrollTop = mk.scrollTop = gutter.scrollTop = ta.scrollTop;
  hl.scrollLeft = mk.scrollLeft = ta.scrollLeft;
  const line = caretLine();
  curLineEl.style.display = st.set.curLine && ta.selectionStart === ta.selectionEnd ? '' : 'none';
  curLineEl.style.top = (10 + (line - 1) * lh() - ta.scrollTop) + 'px';
}
function updateStatus() {
  const p = parse(), s = ta.selectionStart, e = ta.selectionEnd;
  const ln = L.lineOf(p.starts, s), col = s - p.starts[ln - 1] + 1;
  $('#pos').textContent = ln + ':' + col;
  $('#sel').textContent = e > s ? '(' + (e - s) + ' محدّد)' : '';
  $('#count').textContent = p.starts.length + ' سطر · ' + ta.value.length + ' حرف';
}
function updateProblems() {
  const f = cur(), mine = fileDiags(f);
  const all = []; for (const g of st.files) for (const d of fileDiags(g)) all.push({ ...d, file: g.name, fid: g.id });
  X.problems = all;
  const b = $('#stProb');
  b.textContent = all.length ? '✗ ' + all.length + ' ' + (all.length === 1 ? 'مشكلة' : 'مشاكل') : '✓ لا مشاكل';
  b.classList.toggle('has', all.length > 0);
  const c = $('#probCount'); c.hidden = !all.length; c.textContent = all.length;
  for (const t of document.querySelectorAll('.tab')) t.classList.toggle('bad', fileDiags(st.files.find((x) => x.id === +t.dataset.id) || { id: -1, name: '' }).length > 0);
  X.hooks.render.forEach((h) => h('problems'));
  return mine;
}
X.updateProblems = updateProblems;

function renderAll() { renderCode(); renderMarks(); updateStatus(); updateProblems(); }
X.renderAll = renderAll;
let selRaf = 0;
function onSelect() {
  updateStatus();
  if (selRaf) return;
  selRaf = requestAnimationFrame(() => {
    selRaf = 0; renderGutter(); renderMarks();
    X.hooks.select.forEach((h) => h());
  });
}
X.onSelect = onSelect;

/* ------------------------------------------------------------- input -- */
function onInput(e) {
  cur().code = ta.value;
  if (diag.run.length) diag.run = [];
  delete diag.live[cur().id]; // stale until the next check
  persist();
  renderCode(); renderMarks(); updateStatus(); updateProblems();
  scheduleCheck();
  X.hooks.input.forEach((h) => h(e));
}
X.onInput = onInput;

/* -------------------------------------------------------------- tabs -- */
function renderTabs() {
  const list = $('#tabList'); list.textContent = '';
  for (const f of st.files) {
    const d = document.createElement('div');
    d.className = 'tab' + (f.id === st.active ? ' active' : '');
    d.dataset.id = f.id; d.title = f.name;
    const s = document.createElement('span'); s.textContent = f.name; d.appendChild(s);
    const x = document.createElement('button'); x.className = 'x'; x.textContent = '×'; x.setAttribute('aria-label', 'إغلاق'); x.dataset.close = f.id;
    d.appendChild(x); list.appendChild(d);
  }
  const a = list.querySelector('.active'); if (a) a.scrollIntoView({ block: 'nearest', inline: 'nearest' });
  updateProblems();
}
X.renderTabs = renderTabs;
function stashView() { const f = cur(); if (f && ta.value === f.code) { f.sel = [ta.selectionStart, ta.selectionEnd]; f.sy = ta.scrollTop; } }
function openFile(id, keepView) {
  stashView();
  st.active = id; const f = cur();
  ta.value = f.code;
  if (f.sel && !keepView) ta.setSelectionRange(Math.min(f.sel[0], f.code.length), Math.min(f.sel[1], f.code.length)); else ta.setSelectionRange(0, 0);
  ta.scrollTop = keepView ? 0 : (f.sy || 0); ta.scrollLeft = 0;
  renderTabs(); renderAll(); persist();
  if (!(f.id in diag.live)) scheduleCheck(60);
  X.hooks.file.forEach((h) => h(f));
}
X.openFile = openFile;
function uniqueName(base, ignoreId) {
  base = base.trim().replace(/[^\p{L}\p{N}_.\-]/gu, '_') || 'file';
  if (!/\.exb$/i.test(base)) base += '.exb';
  let name = base, i = 2;
  while (st.files.some((f) => f.name === name && f.id !== ignoreId)) name = base.replace(/\.exb$/i, '') + '_' + i++ + '.exb';
  return name;
}
function addFile(name, code) {
  const f = { id: st.nextId++, name: uniqueName(name), code };
  st.files.push(f); openFile(f.id); return f;
}
X.addFile = addFile;
function closeFile(id) {
  const f = st.files.find((x) => x.id === id); if (!f) return;
  if (f.code.trim()) { st.trash.unshift({ name: f.name, code: f.code }); st.trash.length = Math.min(st.trash.length, 15); }
  st.files = st.files.filter((x) => x.id !== id);
  delete diag.live[id];
  if (!st.files.length) st.files.push({ id: st.nextId++, name: 'main.exb', code: '' });
  openFile(st.files.some((x) => x.id === st.active) ? st.active : st.files[Math.max(0, Math.min(st.files.length - 1, st.files.findIndex((x) => x.id === id)))].id, true);
  if (f.code.trim()) toast('أُغلق «' + f.name + '» — يمكن استرجاعه (Ctrl+Shift+T)');
}
X.closeFile = closeFile;
X.reopenClosed = () => {
  const t = st.trash.shift(); if (!t) return toast('لا توجد ملفات مغلقة');
  addFile(t.name, t.code);
};
function renameFile(id) {
  const f = st.files.find((x) => x.id === id); if (!f) return;
  const n = prompt('اسم الملف:', f.name); if (!n || n === f.name) return;
  f.name = uniqueName(n, f.id); renderTabs(); persist();
}
X.renameFile = renameFile;
X.duplicateFile = (id) => { const f = st.files.find((x) => x.id === id) || cur(); if (f.id === st.active) f.code = ta.value; addFile(f.name.replace(/\.exb$/i, '') + '_copy', f.code); };
X.promptNew = () => { const n = prompt('اسم الملف الجديد:', 'util.exb'); if (n) addFile(n, ''); };

/* ------------------------------------------------------------ editing -- */
function ins(text) {
  ta.focus();
  if (!document.execCommand('insertText', false, text)) {
    ta.setRangeText(text, ta.selectionStart, ta.selectionEnd, 'end'); onInput({ inputType: 'insertText' });
  }
}
X.ins = ins;
/* replace [a,b) with text, keeping the native undo stack */
function replaceRange(a, b, text) { ta.focus(); ta.setSelectionRange(a, b); ins(text); }
X.replaceRange = replaceRange;
function setSel(a, b) { ta.focus(); ta.setSelectionRange(a, b === undefined ? a : b); onSelect(); }
X.setSel = setSel;

const PAIRS = { '(': ')', '[': ']', '{': '}', '"': '"', "'": "'" };
const CLOSERS = new Set([')', ']', '}', '"', "'"]);
const WORDCH = /[\p{L}\p{N}_]/u;
function handleChar(ch) {
  const s = ta.selectionStart, e = ta.selectionEnd, v = ta.value;
  if (!st.set.autoClose) {
    return dedentCloser(ch, s, e, v);
  }
  if (CLOSERS.has(ch) && s === e && v[s] === ch) { ta.setSelectionRange(s + 1, s + 1); onSelect(); return true; }
  if (PAIRS[ch]) {
    if (s !== e) { ins(ch + v.slice(s, e) + PAIRS[ch]); return true; }
    const inLit = X.inLiteral(s);
    if (inLit) return false;
    if ((ch === '"' || ch === "'") && WORDCH.test(v[s - 1] || '')) return false;
    if (ch !== '"' && ch !== "'" && WORDCH.test(v[s] || '')) return false;
    ins(ch + PAIRS[ch]); ta.setSelectionRange(s + 1, s + 1); return true;
  }
  return dedentCloser(ch, s, e, v);
}
function dedentCloser(ch, s, e, v) { // typing } ) ] on a whitespace-only line outdents it
  if ((ch === '}' || ch === ')' || ch === ']') && s === e) {
    const ls = v.lastIndexOf('\n', s - 1) + 1, before = v.slice(ls, s);
    if (before && /^ +$/.test(before)) {
      const n = Math.min(st.set.tab, before.length);
      ta.setSelectionRange(s - n, s); ins(ch); return true;
    }
  }
  return false;
}
function newline() {
  const s = ta.selectionStart, e = ta.selectionEnd, v = ta.value;
  const ls = v.lastIndexOf('\n', s - 1) + 1;
  const lineBefore = v.slice(ls, s);
  const base = /^[ \t]*/.exec(lineBefore)[0];
  const open = /[{(\[]\s*$/.test(lineBefore) && !X.inLiteral(s);
  if (open && PAIRS[v[s - 1]] === v[e]) {
    ins('\n' + base + ind() + '\n' + base);
    const p = ta.selectionStart - (1 + base.length); ta.setSelectionRange(p, p);
  } else ins('\n' + base + (open ? ind() : ''));
}
function blockRange() {
  const v = ta.value, s = ta.selectionStart, e = ta.selectionEnd;
  const a = v.lastIndexOf('\n', s - 1) + 1;
  let b = v.indexOf('\n', e > s && v[e - 1] === '\n' ? e - 1 : e); if (b < 0) b = v.length;
  return [a, b];
}
X.blockRange = blockRange;
function indentSel(back) {
  const [a, b] = blockRange(), v = ta.value, s = ta.selectionStart, e = ta.selectionEnd;
  if (!back && s === e) {
    const col = s - (v.lastIndexOf('\n', s - 1) + 1), n = st.set.tab;
    return ins(' '.repeat(n - (col % n)));
  }
  const re = new RegExp('^( {1,' + st.set.tab + '}|\\t)');
  const lines = v.slice(a, b).split('\n');
  const nl = lines.map((l) => back ? l.replace(re, '') : (l ? ind() + l : l));
  const txt = nl.join('\n');
  replaceRange(a, b, txt); ta.setSelectionRange(a, a + txt.length);
}
function toggleComment() {
  const [a, b] = blockRange(), lines = ta.value.slice(a, b).split('\n');
  const all = lines.every((l) => !l.trim() || /^\s*\/\//.test(l));
  const minInd = Math.min(...lines.filter((l) => l.trim()).map((l) => /^\s*/.exec(l)[0].length), 1e9);
  const nl = lines.map((l) => !l.trim() ? l : all ? l.replace(/^(\s*)\/\/ ?/, '$1') : l.slice(0, minInd) + '// ' + l.slice(minInd));
  const txt = nl.join('\n');
  replaceRange(a, b, txt); ta.setSelectionRange(a, a + txt.length);
}
function moveLines(dir) {
  const v = ta.value, [a, b] = blockRange();
  if (dir < 0) {
    if (a === 0) return;
    const pa = v.lastIndexOf('\n', a - 2) + 1, prev = v.slice(pa, a - 1), blk = v.slice(a, b);
    const s0 = ta.selectionStart - a, e0 = ta.selectionEnd - a;
    replaceRange(pa, b, blk + '\n' + prev);
    ta.setSelectionRange(pa + s0, pa + e0);
  } else {
    if (b >= v.length) return;
    let nb = v.indexOf('\n', b + 1); if (nb < 0) nb = v.length;
    const next = v.slice(b + 1, nb), blk = v.slice(a, b);
    const s0 = ta.selectionStart - a, e0 = ta.selectionEnd - a, off = next.length + 1;
    replaceRange(a, nb, next + '\n' + blk);
    ta.setSelectionRange(a + off + s0, a + off + e0);
  }
}
function duplicateLines(up) {
  const [a, b] = blockRange(), blk = ta.value.slice(a, b), s = ta.selectionStart, e = ta.selectionEnd;
  replaceRange(b, b, '\n' + blk);
  if (!up) ta.setSelectionRange(s + blk.length + 1, e + blk.length + 1); else ta.setSelectionRange(s, e);
}
function deleteLines() {
  const v = ta.value, [a, b] = blockRange();
  if (b < v.length) replaceRange(a, b + 1, ''); else if (a > 0) replaceRange(a - 1, b, ''); else replaceRange(a, b, '');
}
X.edit = { indent: indentSel, comment: toggleComment, move: moveLines, dup: duplicateLines, del: deleteLines };

X.formatDoc = () => {
  const src = ta.value, res = L.format(src, ind());
  if (res === src) return toast('الكود منسَّق بالفعل');
  const p = parse(), s = ta.selectionStart, ln = L.lineOf(p.starts, s), col = s - p.starts[ln - 1];
  const keepTop = ta.scrollTop;
  replaceRange(0, src.length, res);
  const ps = L.lineStarts(res), nl = Math.min(ln, ps.length), lineLen = (ps[nl] !== undefined ? ps[nl] - 1 : res.length) - ps[nl - 1];
  const pos = ps[nl - 1] + Math.min(col, lineLen); ta.setSelectionRange(pos, pos); ta.scrollTop = keepTop;
  toast('تم تنسيق الكود');
};

ta.addEventListener('beforeinput', (e) => {
  if (e.isComposing) return;
  const t = e.inputType;
  if (t === 'insertLineBreak' || t === 'insertParagraph') { e.preventDefault(); newline(); }
  else if (t === 'insertText' && e.data && e.data.length === 1 && handleChar(e.data)) e.preventDefault();
  else if (t === 'deleteContentBackward') {
    const s = ta.selectionStart, v = ta.value;
    if (s !== ta.selectionEnd || s === 0) return;
    const ls = v.lastIndexOf('\n', s - 1) + 1, before = v.slice(ls, s);
    if (st.set.autoClose && PAIRS[v[s - 1]] === v[s]) {
      e.preventDefault(); ta.setSelectionRange(s - 1, s + 1); document.execCommand('delete');
    } else if (before.length > 1 && /^ +$/.test(before)) { // smart backspace: back to previous tab stop
      const n = (before.length % st.set.tab) || st.set.tab;
      e.preventDefault(); ta.setSelectionRange(s - n, s); document.execCommand('delete');
    }
  }
});
ta.addEventListener('input', onInput);
ta.addEventListener('scroll', syncScroll);
for (const ev of ['keyup', 'click', 'focus']) ta.addEventListener(ev, onSelect);
document.addEventListener('selectionchange', () => { if (document.activeElement === ta) onSelect(); });

/* --------------------------------------------------------- caret metrics -- */
/* Pixel position of text offset `pos` relative to #codeWrap (accounts for scroll). */
X.caretXY = (pos) => {
  const p = parse(), ln = L.lineOf(p.starts, pos);
  meas.textContent = ta.value.slice(p.starts[ln - 1], pos) || '​';
  const w = meas.getBoundingClientRect().width;
  return { x: 10 + w - ta.scrollLeft, y: 10 + (ln - 1) * lh() - ta.scrollTop, h: lh() };
};
function gotoLine(n, focus, col) {
  const p = parse(); if (n < 1 || n > p.starts.length) return;
  const a = p.starts[n - 1], b = n < p.starts.length ? p.starts[n] - 1 : ta.value.length;
  if (focus) { ta.focus(); if (col) ta.setSelectionRange(a + Math.min(col - 1, b - a), a + Math.min(col - 1, b - a)); else ta.setSelectionRange(a, b); }
  const h = lh(), y = (n - 1) * h;
  if (y < ta.scrollTop + h || y > ta.scrollTop + ta.clientHeight - h * 3) ta.scrollTop = Math.max(0, y - ta.clientHeight / 3);
  syncScroll(); onSelect();
}
X.gotoLine = gotoLine;
X.revealRange = (a, b) => { // select and scroll a range into view
  ta.focus(); ta.setSelectionRange(a, b);
  const ln = L.lineOf(parse().starts, a), h = lh(), y = (ln - 1) * h;
  if (y < ta.scrollTop + h || y > ta.scrollTop + ta.clientHeight - h * 3) ta.scrollTop = Math.max(0, y - ta.clientHeight / 3);
  const xy = X.caretXY(a); if (xy.x < 20 || xy.x > ta.clientWidth - 40) ta.scrollLeft = Math.max(0, ta.scrollLeft + xy.x - ta.clientWidth / 3);
  syncScroll(); onSelect();
};

/* -------------------------------------------------------------- runner -- */
/* Normally `new Worker('worker.js')`. Opened straight from disk (file://) browsers refuse that,
   so inline.js (loaded only then) provides the worker source + embedded wasm as a Blob worker. */
function newWorker() {
  if (self.EXB_INLINE && (location.protocol === 'file:' || !/^https?:$/.test(location.protocol))) {
    return new Worker(URL.createObjectURL(new Blob([self.EXB_INLINE.worker], { type: 'text/javascript' })));
  }
  try { return new Worker('worker.js'); }
  catch (e) { if (self.EXB_INLINE) return new Worker(URL.createObjectURL(new Blob([self.EXB_INLINE.worker], { type: 'text/javascript' }))); throw e; }
}
let worker = null, running = false, reqId = 0, runStart = 0, ticker = 0, runCmd = 'run', runFile = null;
function spawn() {
  if (worker) worker.terminate();
  worker = newWorker();
  worker.onmessage = onWorker;
  worker.onerror = (e) => { setRunning(false); showOut('تعذّر تشغيل المحرك: ' + (e.message || ''), 3); };
  worker.postMessage({ id: 0, cmd: 'init' });
}
function setRunning(on) {
  running = on;
  $('#btnRun').hidden = on; $('#btnDisasm').disabled = on; $('#btnStop').hidden = !on;
  clearInterval(ticker);
  if (on) { runStart = performance.now(); ticker = setInterval(() => { $('#outMeta').textContent = '⏳ ' + ((performance.now() - runStart) / 1000).toFixed(1) + 's'; }, 100); }
}
const LINK = /\[([^\]:\n]+):(\d+)\]/g;
function linkify(text) {
  return esc(text).replace(LINK, (m, f, n) => '<a class="ln" data-file="' + f + '" data-line="' + n + '">' + m + '</a>');
}
function showOut(text, status, label) {
  X.showPane('out');
  const MAX = 200000;
  let t = text; if (t.length > MAX) t = t.slice(0, MAX) + '\n… (قُطع الناتج)';
  out.textContent = '';
  if (label) { const h = document.createElement('div'); h.className = 'hint'; h.textContent = '— ' + label + ' —'; out.appendChild(h); }
  const d = document.createElement('div');
  if (status) d.className = 'err';
  d.innerHTML = linkify(t.replace(/\n$/, ''));
  out.appendChild(d); out.scrollTop = label ? 0 : out.scrollHeight;
}
X.showOut = showOut;
const fileByRunName = (name) => name === 'main.exb' ? st.files.find((f) => f.id === runFile) : st.files.find((f) => f.name === name);
function onWorker(ev) {
  const m = ev.data;
  if (m.ready) return;
  if (m.id !== reqId) return;
  setRunning(false);
  let text = m.out || '';
  if (m.exit !== null && m.exit !== undefined) text += (text && !text.endsWith('\n') ? '\n' : '') + '↪ exit(' + m.exit + ')';
  showOut(text || '(لا ناتج)', m.status, runCmd === 'disasm' ? 'bytecode' : '');
  $('#outMeta').textContent = (m.status ? '✗ ' : '✓ ') + Math.round(m.ms) + 'ms';
  if (m.version) $('#ver').textContent = 'EXBIN ' + m.version + ' · wasm';
  diag.run = [];
  if (m.status) {
    for (const d of L.parseDiagnostics(m.out || '')) {
      const f = fileByRunName(d.file); if (f) diag.run.push({ file: f.name, line: d.line, msg: d.msg });
    }
    const first = diag.run[0], f0 = first && st.files.find((f) => f.name === first.file);
    if (f0 && f0.id === st.active) { renderAll(); gotoLine(first.line, false); } else if (f0) { renderAll(); }
    else renderAll();
  } else renderAll();
  X.hooks.run.forEach((h) => h(m));
}
function runCode(cmd) {
  if (running) return;
  cur().code = ta.value; persist();
  diag.run = []; runCmd = cmd; runFile = cur().id;
  renderAll();
  const files = {}; for (const f of st.files) files[f.name] = f.code;
  setRunning(true);
  out.textContent = '';
  X.showPane('out');
  worker.postMessage({ id: ++reqId, cmd, src: ta.value, files, stdin: st.stdin });
  if (matchMedia('(max-width: 899px)').matches) $('#outPane').scrollIntoView({ block: 'nearest' });
}
X.runCode = runCode;
X.stopCode = () => { if (!running) return; setRunning(false); reqId++; spawn(); showOut('■ أُوقف التنفيذ.', 0); $('#outMeta').textContent = ''; };
X.isRunning = () => running;
out.addEventListener('click', (e) => {
  const a = e.target.closest('a.ln'); if (!a) return;
  const f = fileByRunName(a.dataset.file);
  if (f && f.id !== st.active) openFile(f.id);
  gotoLine(+a.dataset.line, true);
});

/* ---------------------------------------------------- live syntax check -- */
let chk = null, chkId = 0, chkTimer = 0, chkBusy = false, chkPending = false, chkFile = 0;
function spawnCheck() {
  if (chk) chk.terminate();
  chk = newWorker(); chkBusy = false;
  chk.onmessage = (ev) => {
    const m = ev.data; if (m.ready) return;
    chkBusy = false;
    if (m.id === chkId) {
      const f = st.files.find((x) => x.id === chkFile);
      if (f) {
        diag.live[f.id] = m.status ? L.parseDiagnostics(m.out || '').filter((d) => d.file === 'main.exb').map((d) => ({ line: d.line, msg: d.msg })) : [];
        if (f.id === st.active && f.code === ta.value) { renderMarks(); renderGutter(); }
        updateProblems();
      }
    }
    if (chkPending) { chkPending = false; doCheck(); }
  };
  chk.onerror = () => { chkBusy = false; };
  chk.postMessage({ id: 0, cmd: 'init' });
}
function doCheck() {
  if (!chk || !st.set.live) return;
  if (chkBusy) { chkPending = true; return; }
  chkBusy = true; chkFile = st.active;
  chk.postMessage({ id: ++chkId, cmd: 'check', src: ta.value });
}
function scheduleCheck(ms) { if (!st.set.live) return; clearTimeout(chkTimer); chkTimer = setTimeout(doCheck, ms || 450); }
X.scheduleCheck = scheduleCheck;
X.setLive = (on) => { st.set.live = on; if (!on) { diag.live = {}; renderAll(); } else scheduleCheck(50); };

/* ------------------------------------------------------------- misc API -- */
X.applyTheme = () => {
  const dark = st.theme ? st.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  $('meta[name="theme-color"]').content = dark ? '#12152e' : '#eef0fb';
};
X.applyFont = () => {
  const r = document.documentElement.style;
  r.setProperty('--fs', st.fs + 'px'); r.setProperty('--tab', st.set.tab);
  lastGutterKey = ''; renderAll();
};
X.download = (name, data, type) => {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([data], { type: type || 'text/plain;charset=utf-8' }));
  a.download = name; document.body.appendChild(a); a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
};
X.syncCode = () => { cur().code = ta.value; };
X.start = () => {
  X.applyTheme();
  matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change', () => { if (!st.theme) X.applyTheme(); });
  const r = document.documentElement.style; r.setProperty('--fs', st.fs + 'px'); r.setProperty('--tab', st.set.tab);
  const f = cur();
  ta.value = f.code;
  if (f.sel) ta.setSelectionRange(Math.min(f.sel[0], f.code.length), Math.min(f.sel[1], f.code.length));
  renderTabs(); renderAll();
  ta.scrollTop = f.sy || 0; syncScroll();
  out.innerHTML = '<div class="hint">اضغط «تشغيل» أو Ctrl+Enter لتنفيذ الكود — F1 للوحة الأوامر.</div>';
  spawn(); spawnCheck(); scheduleCheck(200);
  window.addEventListener('beforeunload', () => { stashView(); cur().code = ta.value; safe(() => localStorage.setItem(KEY, JSON.stringify(st))); });
  window.addEventListener('pagehide', () => { stashView(); cur().code = ta.value; safe(() => localStorage.setItem(KEY, JSON.stringify(st))); });
};
window.__exbin = { runCode, st: () => st, X };
})();
