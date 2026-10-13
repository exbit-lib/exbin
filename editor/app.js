/* EXBIN editor — dependency-free, offline-first. */
(() => {
'use strict';
const $ = (s) => document.querySelector(s);
const DATA = self.EXB_DATA;

/* ------------------------------------------------------------ language -- */
const KEYWORDS = new Set('and break continue else fn for if in let not or return while'.split(' '));
const CONSTS = new Set('true false nil PI E INF EXBIN_VERSION'.split(' '));
const BUILTINS = new Set(('print write input read_file read_bytes write_file append_file file_exists env exit args type str repr num int list bytes format ' +
  'len push pop insert remove delete keys values items has find copy slice join reverse sort map filter reduce each any all sum zip enumerate range ' +
  'split lines chars replace upper lower trim trim_start trim_end starts_with ends_with repeat pad_left pad_right substr chr ord utf8_len ' +
  'abs floor ceil round sqrt sign sin cos tan asin acos atan atan2 log log2 log10 exp pow min max clamp random seed hex bin ' +
  'error assert pcall apply require getglobal setglobal defined callable clock time sleep ' +
  'lexer regex asm compile eval disasm opcodes fn_info serialize deserialize save_bytecode load_bytecode to_bytes to_hex to_string').split(' '));

const TOKEN = /(\/\/[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|("(?:\\[\s\S]|[^"\\\n])*"?|'(?:\\[\s\S]|[^'\\\n])*'?)|(0[xX][0-9a-fA-F]+|\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?)|([\p{L}_][\p{L}\p{N}_]*)|(\s+|[\s\S])/gu;
const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;' };
const esc = (s) => s.replace(/[&<>]/g, (c) => ESC[c]);

function highlight(code, errLine) {
  let out = '', line = 1, prevWord = '', m;
  const emit = (text, cls) => {
    const parts = text.split('\n');
    for (let i = 0; i < parts.length; i++) {
      if (i > 0) { out += '\n'; line++; }
      let p = parts[i];
      if (!p) continue;
      p = esc(p);
      if (line === errLine && p.trim()) p = '<span class="e">' + p + '</span>';
      out += cls ? '<span class="' + cls + '">' + p + '</span>' : p;
    }
  };
  TOKEN.lastIndex = 0;
  while ((m = TOKEN.exec(code))) {
    if (m[1] !== undefined) emit(m[1], 'c');
    else if (m[2] !== undefined) emit(m[2], 's');
    else if (m[3] !== undefined) emit(m[3], 'n');
    else if (m[4] !== undefined) {
      const w = m[4];
      let cls = '';
      if (KEYWORDS.has(w)) cls = 'k';
      else if (CONSTS.has(w)) cls = 't';
      else if (prevWord === 'fn') cls = 'f';
      else if (BUILTINS.has(w)) cls = 'b';
      else if (code[TOKEN.lastIndex] === '(') cls = 'f';
      emit(w, cls);
      prevWord = w;
      continue;
    } else {
      emit(m[5], '');
      if (m[5].trim()) prevWord = '';
      continue;
    }
    prevWord = '';
  }
  return out + '\n ';
}

/* --------------------------------------------------------------- state -- */
const KEY = 'exbin.editor.v1';
const safe = (f, d) => { try { return f(); } catch (_) { return d; } };
let st = safe(() => JSON.parse(localStorage.getItem(KEY)), null);
if (!st || !Array.isArray(st.files) || !st.files.length) {
  st = { files: [{ id: 1, name: 'main.exb', code: DATA.examples[0].code }], active: 1, theme: null, fs: 15, nextId: 2 };
}
st.fs = st.fs || 15;
let saveTimer = 0;
const persist = () => { clearTimeout(saveTimer); saveTimer = setTimeout(() => safe(() => localStorage.setItem(KEY, JSON.stringify(st))), 300); };
const cur = () => st.files.find((f) => f.id === st.active) || st.files[0];

/* ------------------------------------------------------------ elements -- */
const ta = $('#ta'), hl = $('#hl'), gutter = $('#gutter'), out = $('#out');
let errLine = 0, lastLines = 0, lastErr = -1;

function toast(msg) {
  const t = $('#toast'); t.textContent = msg; t.hidden = false;
  clearTimeout(toast.t); toast.t = setTimeout(() => { t.hidden = true; }, 2200);
}

/* -------------------------------------------------------------- render -- */
function render() {
  hl.innerHTML = highlight(ta.value, errLine);
  const n = ta.value.split('\n').length;
  if (n !== lastLines || errLine !== lastErr) {
    let g = '';
    for (let i = 1; i <= n; i++) g += (i === errLine ? '<span class="err">' + i + '</span>' : i) + '\n';
    gutter.innerHTML = g;
    lastLines = n; lastErr = errLine;
  }
  syncScroll();
  updateStatus();
}
function syncScroll() {
  hl.scrollTop = gutter.scrollTop = ta.scrollTop;
  hl.scrollLeft = ta.scrollLeft;
}
function updateStatus() {
  const v = ta.value, p = ta.selectionStart;
  const before = v.slice(0, p);
  const ln = before.split('\n').length, col = p - before.lastIndexOf('\n');
  $('#pos').textContent = ln + ':' + col;
  $('#count').textContent = lastLines + ' سطر · ' + v.length + ' حرف';
}
function onInput() {
  cur().code = ta.value;
  if (errLine) errLine = 0;
  persist();
  render();
}

/* ----------------------------------------------------------------- tabs -- */
function renderTabs() {
  const list = $('#tabList'); list.textContent = '';
  for (const f of st.files) {
    const d = document.createElement('div');
    d.className = 'tab' + (f.id === st.active ? ' active' : '');
    d.dataset.id = f.id;
    const s = document.createElement('span'); s.textContent = f.name; d.appendChild(s);
    const x = document.createElement('button'); x.className = 'x'; x.textContent = '×'; x.setAttribute('aria-label', 'إغلاق'); x.dataset.close = f.id;
    d.appendChild(x); list.appendChild(d);
  }
  const a = list.querySelector('.active'); if (a) a.scrollIntoView({ block: 'nearest', inline: 'nearest' });
}
function openFile(id) {
  st.active = id; const f = cur();
  ta.value = f.code; errLine = 0; ta.scrollTop = ta.scrollLeft = 0;
  renderTabs(); render(); persist();
}
function uniqueName(base) {
  base = base.replace(/[^\p{L}\p{N}_.\-]/gu, '_') || 'file';
  if (!/\.exb$/i.test(base)) base += '.exb';
  let name = base, i = 2;
  while (st.files.some((f) => f.name === name)) name = base.replace(/\.exb$/i, '') + '_' + i++ + '.exb';
  return name;
}
function addFile(name, code) {
  const f = { id: st.nextId++, name: uniqueName(name), code };
  st.files.push(f); openFile(f.id); return f;
}
function closeFile(id) {
  const f = st.files.find((x) => x.id === id); if (!f) return;
  if (f.code.trim() && !confirm('إغلاق «' + f.name + '»؟ سيُحذف من هذا الجهاز.')) return;
  st.files = st.files.filter((x) => x.id !== id);
  if (!st.files.length) st.files.push({ id: st.nextId++, name: 'main.exb', code: '' });
  openFile(st.files.some((x) => x.id === st.active) ? st.active : st.files[0].id);
}
function renameFile(id) {
  const f = st.files.find((x) => x.id === id); if (!f) return;
  const n = prompt('اسم الملف:', f.name); if (!n || n === f.name) return;
  f.name = '__tmp__'; f.name = uniqueName(n); renderTabs(); persist();
}
$('#tabList').addEventListener('click', (e) => {
  const c = e.target.closest('[data-close]');
  if (c) return closeFile(+c.dataset.close);
  const t = e.target.closest('.tab'); if (t) openFile(+t.dataset.id);
});
$('#tabList').addEventListener('dblclick', (e) => { const t = e.target.closest('.tab'); if (t) renameFile(+t.dataset.id); });
(() => { // long-press to rename (touch)
  let timer = 0;
  const l = $('#tabList');
  l.addEventListener('pointerdown', (e) => { const t = e.target.closest('.tab'); if (!t || e.target.closest('.x')) return; timer = setTimeout(() => renameFile(+t.dataset.id), 700); });
  for (const ev of ['pointerup', 'pointerleave', 'pointercancel', 'scroll']) l.addEventListener(ev, () => clearTimeout(timer));
})();
$('#btnAdd').addEventListener('click', () => {
  const n = prompt('اسم الملف الجديد:', 'util.exb'); if (n) addFile(n, '');
});

/* ------------------------------------------------------------- editing -- */
function ins(text) {
  ta.focus();
  if (!document.execCommand('insertText', false, text)) {
    ta.setRangeText(text, ta.selectionStart, ta.selectionEnd, 'end'); onInput();
  }
}
const PAIRS = { '(': ')', '[': ']', '{': '}', '"': '"', "'": "'" };
const CLOSERS = new Set([')', ']', '}', '"', "'"]);
function handleChar(ch) {
  const s = ta.selectionStart, e = ta.selectionEnd, v = ta.value;
  if (CLOSERS.has(ch) && s === e && v[s] === ch) { ta.setSelectionRange(s + 1, s + 1); return true; }
  if (PAIRS[ch]) {
    if (s !== e) { ins(ch + v.slice(s, e) + PAIRS[ch]); return true; }
    if ((ch === '"' || ch === "'") && /[\p{L}\p{N}_]/u.test(v[s - 1] || '')) return false;
    if (ch !== '"' && ch !== "'" && /[\p{L}\p{N}_]/u.test(v[s] || '')) return false;
    ins(ch + PAIRS[ch]); ta.setSelectionRange(s + 1, s + 1); return true;
  }
  return false;
}
function newline() {
  const s = ta.selectionStart, e = ta.selectionEnd, v = ta.value;
  const ls = v.lastIndexOf('\n', s - 1) + 1;
  const lineBefore = v.slice(ls, s);
  const ind = /^[ \t]*/.exec(lineBefore)[0];
  const open = /[{(\[]\s*$/.test(lineBefore);
  if (open && PAIRS[v[s - 1]] === v[e]) {
    ins('\n' + ind + '  \n' + ind);
    const p = ta.selectionStart - (1 + ind.length); ta.setSelectionRange(p, p);
  } else ins('\n' + ind + (open ? '  ' : ''));
}
function blockRange() {
  const v = ta.value, s = ta.selectionStart, e = ta.selectionEnd;
  const a = v.lastIndexOf('\n', s - 1) + 1;
  let b = v.indexOf('\n', e > s && v[e - 1] === '\n' ? e - 1 : e); if (b < 0) b = v.length;
  return [a, b];
}
function indentSel(back) {
  const [a, b] = blockRange(), v = ta.value, s = ta.selectionStart, e = ta.selectionEnd;
  if (!back && s === e) return ins('  ');
  const lines = v.slice(a, b).split('\n');
  const nl = lines.map((l) => back ? l.replace(/^( {1,2}|\t)/, '') : '  ' + l);
  ta.setSelectionRange(a, b); ins(nl.join('\n'));
  ta.setSelectionRange(a, a + nl.join('\n').length);
}
function toggleComment() {
  const [a, b] = blockRange(), lines = ta.value.slice(a, b).split('\n');
  const all = lines.every((l) => !l.trim() || /^\s*\/\//.test(l));
  const nl = lines.map((l) => !l.trim() ? l : all ? l.replace(/^(\s*)\/\/ ?/, '$1') : l.replace(/^(\s*)/, '$1// '));
  ta.setSelectionRange(a, b); ins(nl.join('\n'));
  ta.setSelectionRange(a, a + nl.join('\n').length);
}
ta.addEventListener('beforeinput', (e) => {
  if (e.isComposing) return;
  const t = e.inputType;
  if (t === 'insertLineBreak' || t === 'insertParagraph') { e.preventDefault(); newline(); }
  else if (t === 'insertText' && e.data && e.data.length === 1 && handleChar(e.data)) e.preventDefault();
  else if (t === 'deleteContentBackward') {
    const s = ta.selectionStart, v = ta.value;
    if (s === ta.selectionEnd && s > 0 && PAIRS[v[s - 1]] === v[s]) {
      e.preventDefault(); ta.setSelectionRange(s - 1, s + 1); document.execCommand('delete');
    }
  }
});
ta.addEventListener('keydown', (e) => {
  const mod = e.ctrlKey || e.metaKey;
  if (e.key === 'Tab' && !mod) { e.preventDefault(); indentSel(e.shiftKey); }
  else if (mod && e.key === 'Enter') { e.preventDefault(); runCode('run'); }
  else if (e.key === 'F5') { e.preventDefault(); runCode('run'); }
  else if (mod && (e.key === 's' || e.key === 'S')) { e.preventDefault(); downloadCurrent(); }
  else if (mod && e.key === '/') { e.preventDefault(); toggleComment(); }
});
ta.addEventListener('input', onInput);
ta.addEventListener('scroll', syncScroll);
for (const ev of ['keyup', 'click', 'focus']) ta.addEventListener(ev, updateStatus);
document.addEventListener('selectionchange', () => { if (document.activeElement === ta) updateStatus(); });

/* -------------------------------------------------------- symbols bar -- */
(() => {
  const bar = $('#keys');
  const keys = [['↹', 'tab'], ['↶', 'undo'], ['↷', 'redo'], '{', '}', '(', ')', '[', ']', '"', "'", '=', '+', '-', '*', '/', '%', ';', ':', '.', ',', '!', '<', '>', '&', '|', '_'];
  for (const k of keys) {
    const b = document.createElement('button');
    b.type = 'button';
    b.textContent = Array.isArray(k) ? k[0] : k; b.dataset.k = Array.isArray(k) ? k[1] : k;
    bar.appendChild(b);
  }
  const keep = (e) => e.preventDefault();
  bar.addEventListener('pointerdown', keep); bar.addEventListener('mousedown', keep);
  bar.addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    const k = b.dataset.k; ta.focus();
    if (k === 'tab') indentSel(false);
    else if (k === 'undo') document.execCommand('undo');
    else if (k === 'redo') document.execCommand('redo');
    else if (!handleChar(k)) ins(k);
  });
})();

/* -------------------------------------------------------------- runner -- */
let worker = null, running = false, reqId = 0, runStart = 0, ticker = 0, ready = false;
function spawn() {
  if (worker) worker.terminate();
  ready = false;
  worker = new Worker('worker.js');
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
function linkify(text) {
  return esc(text).replace(/\[main\.exb:(\d+)\]/g, (m, n) => '<a class="ln" data-line="' + n + '">' + m + '</a>');
}
function showOut(text, status, title) {
  $('#outTitle').textContent = title || 'الناتج';
  const MAX = 200000;
  let t = text; if (t.length > MAX) t = t.slice(0, MAX) + '\n… (قُطع الناتج)';
  const d = document.createElement('div');
  if (status) d.className = 'err';
  d.innerHTML = linkify(t.replace(/\n$/, ''));
  out.textContent = ''; out.appendChild(d); out.scrollTop = out.scrollHeight;
}
function onWorker(ev) {
  const m = ev.data;
  if (m.ready) { ready = true; return; }
  if (m.id !== reqId) return;
  setRunning(false);
  const title = m.cmd === 'disasm' ? 'bytecode' : 'الناتج';
  let text = m.out || '';
  if (m.exit !== null && m.exit !== undefined) text += (text && !text.endsWith('\n') ? '\n' : '') + '↪ exit(' + m.exit + ')';
  showOut(text || '(لا ناتج)', m.status, lastCmd === 'disasm' ? 'bytecode' : title);
  $('#outMeta').textContent = (m.status ? '✗ ' : '✓ ') + Math.round(m.ms) + 'ms';
  if (m.version) $('#ver').textContent = 'EXBIN ' + m.version + ' · wasm';
  if (m.status) {
    const mm = /\[main\.exb:(\d+)\]/.exec(m.out || '');
    if (mm) { errLine = +mm[1]; render(); gotoLine(errLine, false); }
  }
}
let lastCmd = 'run';
function runCode(cmd) {
  if (running) return;
  cur().code = ta.value; persist();
  errLine = 0; render();
  lastCmd = cmd;
  const files = {}; for (const f of st.files) files[f.name] = f.code;
  setRunning(true);
  $('#outTitle').textContent = cmd === 'disasm' ? 'bytecode' : 'الناتج';
  out.textContent = '';
  worker.postMessage({ id: ++reqId, cmd, src: ta.value, files });
  if (matchMedia('(max-width: 899px)').matches) out.scrollIntoView({ block: 'nearest' });
}
function stopCode() {
  if (!running) return;
  setRunning(false); reqId++;
  spawn(); showOut('■ أُوقف التنفيذ.', 0);
}
function gotoLine(n, focus) {
  const lines = ta.value.split('\n'); if (n < 1 || n > lines.length) return;
  let p = 0; for (let i = 1; i < n; i++) p += lines[i - 1].length + 1;
  if (focus) { ta.focus(); ta.setSelectionRange(p, p + lines[n - 1].length); }
  const lh = parseFloat(getComputedStyle(ta).lineHeight) || 24;
  const y = (n - 1) * lh;
  if (y < ta.scrollTop || y > ta.scrollTop + ta.clientHeight - lh * 2) ta.scrollTop = Math.max(0, y - ta.clientHeight / 3);
  syncScroll();
}
out.addEventListener('click', (e) => { const a = e.target.closest('a.ln'); if (a) gotoLine(+a.dataset.line, true); });
$('#btnRun').addEventListener('click', () => runCode('run'));
$('#btnDisasm').addEventListener('click', () => runCode('disasm'));
$('#btnStop').addEventListener('click', stopCode);
$('#btnClear').addEventListener('click', () => { out.textContent = ''; $('#outMeta').textContent = ''; });

/* -------------------------------------------------------------- files -- */
function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'text/plain;charset=utf-8' }));
  a.download = name; document.body.appendChild(a); a.click();
  setTimeout(() => { URL.revokeObjectURL(a.href); a.remove(); }, 1000);
}
function downloadCurrent() { cur().code = ta.value; persist(); download(cur().name, cur().code); toast('نُزّل «' + cur().name + '»'); }
$('#fileIn').addEventListener('change', async (e) => {
  for (const f of e.target.files) addFile(f.name, await f.text());
  e.target.value = '';
});

/* ---------------------------------------------------------------- menu -- */
const menu = $('#menu');
const ex = $('#examples');
for (const e of DATA.examples) {
  const b = document.createElement('button'); b.dataset.ex = e.name; b.textContent = e.title; ex.appendChild(b);
}
let deferredInstall = null;
window.addEventListener('beforeinstallprompt', (e) => { e.preventDefault(); deferredInstall = e; $('[data-act="install"]').hidden = false; });
window.addEventListener('appinstalled', () => { $('[data-act="install"]').hidden = true; });
function applyTheme() {
  const dark = st.theme ? st.theme === 'dark' : matchMedia('(prefers-color-scheme: dark)').matches;
  document.documentElement.dataset.theme = dark ? 'dark' : 'light';
  $('meta[name="theme-color"]').content = dark ? '#12152e' : '#eef0fb';
}
function applyFont() { document.documentElement.style.setProperty('--fs', st.fs + 'px'); lastLines = -1; if (ta.value !== undefined) render(); }
function about() {
  $('#dlgBody').innerHTML = '<h3 style="margin:0 0 8px">محرر EXBIN</h3><p style="margin:0 0 6px">يعمل بالكامل على جهازك وبدون إنترنت: محرك EXBIN مُترجَم إلى WebAssembly.</p>' +
    '<p style="margin:0 0 6px;color:var(--muted)">اختصارات: <kbd>Ctrl</kbd>+<kbd>Enter</kbd> تشغيل · <kbd>Ctrl</kbd>+<kbd>S</kbd> تنزيل · <kbd>Ctrl</kbd>+<kbd>/</kbd> تعليق · <kbd>Tab</kbd> إزاحة.</p>' +
    '<p style="margin:0;color:var(--muted)">الملفات المفتوحة تعمل مع <code>require("اسم")</code>، وللمكتبتين <code>pratt</code> و<code>pretty</code> جاهزتان.</p>';
  $('#dlg').showModal();
}
function closeMenu() { menu.hidden = true; }
$('#btnMenu').addEventListener('click', (e) => { e.stopPropagation(); menu.hidden = !menu.hidden; });
document.addEventListener('click', (e) => { if (!menu.hidden && !menu.contains(e.target)) closeMenu(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeMenu(); });
menu.addEventListener('click', (e) => {
  const b = e.target.closest('button'); if (!b) return;
  const act = b.dataset.act;
  if (b.dataset.ex) { const x = DATA.examples.find((z) => z.name === b.dataset.ex); addFile(x.name, x.code); closeMenu(); return; }
  if (act === 'fsMinus' || act === 'fsPlus') { st.fs = Math.max(11, Math.min(26, st.fs + (act === 'fsPlus' ? 1 : -1))); applyFont(); persist(); return; }
  closeMenu();
  if (act === 'new') $('#btnAdd').click();
  else if (act === 'open') $('#fileIn').click();
  else if (act === 'save') downloadCurrent();
  else if (act === 'theme') { st.theme = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'; applyTheme(); persist(); }
  else if (act === 'install' && deferredInstall) { deferredInstall.prompt(); deferredInstall = null; }
  else if (act === 'about') about();
});

/* ------------------------------------------------------------ splitter -- */
(() => {
  const bar = $('#gutterBar'), split = $('#split'), pane = $('#editorPane'), outp = $('#outPane');
  let drag = false;
  bar.addEventListener('pointerdown', (e) => { drag = true; bar.setPointerCapture(e.pointerId); });
  bar.addEventListener('pointerup', () => { drag = false; });
  bar.addEventListener('pointermove', (e) => {
    if (!drag) return;
    const r = split.getBoundingClientRect();
    const wide = matchMedia('(min-width: 900px)').matches;
    let f = wide ? (e.clientX - r.left) / r.width : (e.clientY - r.top) / r.height;
    f = Math.max(0.15, Math.min(0.85, f));
    pane.style.flex = '0 0 ' + (f * 100) + '%'; outp.style.flex = '1 1 0';
  });
})();

/* ----------------------------------------------------------------- boot -- */
applyTheme();
matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change', () => { if (!st.theme) applyTheme(); });
document.documentElement.style.setProperty('--fs', st.fs + 'px');
ta.value = cur().code;
renderTabs(); render();
out.innerHTML = '<div class="hint">اضغط «تشغيل» أو Ctrl+Enter لتنفيذ الكود.</div>';
spawn();
window.addEventListener('beforeunload', () => { cur().code = ta.value; safe(() => localStorage.setItem(KEY, JSON.stringify(st))); });
if ('serviceWorker' in navigator && /^https?:$/.test(location.protocol)) navigator.serviceWorker.register('sw.js').catch(() => {});
window.__exbin = { runCode, st: () => st };
})();
