/* Autocomplete popup + signature help. */
(() => {
'use strict';
const X = self.EXB, L = X.L, $ = X.$, ta = X.ta, st = X.st;
const ac = $('#ac'), sig = $('#sig');
const WORD = /[\p{L}\p{N}_]/u;
const A = { open: false, items: [], idx: 0, start: 0, member: false };

/* --------------------------------------------------------- candidates -- */
function projectIndex() { // names declared in the other open files
  const m = new Map();
  for (const f of st.files) {
    if (f.id === st.active) continue;
    if (f._c !== f.code) { f._c = f.code; f._s = L.symbols(f.code); }
    for (const s of f._s) if (!m.has(s.name)) m.set(s.name, { name: s.name, kind: s.kind === 'fn' ? 'fn' : 'v', sig: s.kind === 'fn' ? s.name + '(' + s.params + ')' : 'let', doc: 'من ' + f.name });
  }
  return m;
}
function score(q, name) { // higher is better, -1 = no match
  if (!q) return 1;
  const a = q.toLowerCase(), b = name.toLowerCase();
  if (b === a) return 100;
  if (b.startsWith(a)) return 90 - Math.min(20, name.length - q.length) * 0.5;
  const i = b.indexOf(a); if (i > 0) return (name[i - 1] === '_' ? 70 : 50) - i * 0.1;
  let j = 0; for (const c of b) if (c === a[j]) j++; // subsequence
  return j === a.length ? 20 : -1;
}
function candidates(prefix, member) {
  const p = X.parse(), seen = new Map(), add = (it) => { if (!seen.has(it.name) || it.rank < seen.get(it.name).rank) seen.set(it.name, it); };
  if (member) {
    for (const m of L.METHODS) add({ name: m.name, kind: 'm', sig: m.sig, doc: m.doc, rank: 1 });
    for (const r of L.REF) add({ name: r.name, kind: 'm', sig: r.sig, doc: r.doc, rank: 2 });
  } else {
    for (const k of L.KEYWORDS) add({ name: k, kind: 'k', sig: 'كلمة مفتاحية', rank: 5 });
    for (const c of L.CONSTS) add({ name: c, kind: 'v', sig: 'ثابت', rank: 4 });
    for (const sn of L.SNIPPETS) add({ name: sn.name, kind: 's', sig: sn.sig, snippet: sn.body, rank: 6 });
    for (const r of L.REF) add({ name: r.name, kind: 'fn', sig: r.sig, doc: r.doc, rank: 3 });
    for (const [n, it] of projectIndex()) add({ ...it, rank: 1 });
    const cnt = L.words(p.code, p.toks);
    for (const [w, c] of cnt) {
      if (w === prefix && c < 2) continue;
      if (!seen.has(w) || seen.get(w).rank > 2) add({ name: w, kind: 'v', sig: 'محلي', rank: 2 });
    }
    for (const s of p.syms) add({ name: s.name, kind: s.kind === 'fn' ? 'fn' : 'v', sig: s.kind === 'fn' ? s.name + '(' + s.params + ')' : 'let', rank: 0 });
  }
  const out = [];
  for (const it of seen.values()) {
    const sc = score(prefix, it.name); if (sc < 0) continue;
    out.push({ ...it, sc: sc - it.rank * 0.01 });
  }
  out.sort((a, b) => b.sc - a.sc || a.name.localeCompare(b.name));
  return out.slice(0, 40);
}

/* ------------------------------------------------------------- popup -- */
function wordBounds(pos) {
  const v = ta.value; let s = pos;
  while (s > 0 && WORD.test(v[s - 1])) s--;
  return { s, prefix: v.slice(s, pos), member: v[s - 1] === '.' && !/\d/.test(v[s - 2] || '') };
}
const ICON = { fn: 'ƒ', m: 'ƒ', v: 'x', k: '◆', s: '▸' };
function hi(name, q) { // bold the matched prefix
  if (!q || !name.toLowerCase().startsWith(q.toLowerCase())) return X.L.esc(name);
  return '<b>' + X.L.esc(name.slice(0, q.length)) + '</b>' + X.L.esc(name.slice(q.length));
}
function render() {
  ac.textContent = '';
  const q = ta.value.slice(A.start, ta.selectionStart);
  A.items.forEach((it, i) => {
    const d = document.createElement('div'); d.className = 'it' + (i === A.idx ? ' on' : ''); d.dataset.i = i; d.setAttribute('role', 'option');
    d.innerHTML = '<span class="ik ' + it.kind + '">' + ICON[it.kind] + '</span><span class="nm">' + hi(it.name, q) + '</span><span class="dt">' + X.L.esc(it.kind === 's' ? it.sig : it.sig.replace(/^[^(]*/, '') || it.sig) + '</span>';
    ac.appendChild(d);
  });
  const cur = A.items[A.idx];
  if (cur && cur.doc) { const d = document.createElement('div'); d.className = 'doc'; d.textContent = cur.doc; ac.appendChild(d); }
  const on = ac.querySelector('.it.on'); if (on) on.scrollIntoView({ block: 'nearest' });
}
function place() {
  const xy = X.caretXY(A.start), wrap = $('#codeWrap');
  ac.hidden = false;
  const w = ac.offsetWidth, h = ac.offsetHeight, W = wrap.clientWidth, H = wrap.clientHeight;
  let x = Math.max(4, Math.min(xy.x, W - w - 4)), y = xy.y + xy.h + 2;
  if (y + h > H - 4) y = Math.max(4, xy.y - h - 2);
  ac.style.left = x + 'px'; ac.style.top = y + 'px';
}
function close() { if (!A.open) return; A.open = false; ac.hidden = true; }
function show(force) {
  if (!st.set.suggest && !force) return close();
  const pos = ta.selectionStart;
  if (pos !== ta.selectionEnd) return close();
  if (X.inLiteral(pos)) return close();
  const w = wordBounds(pos);
  if (!force && !w.prefix && !w.member) return close();
  if (!force && /^\d/.test(w.prefix)) return close();
  const items = candidates(w.prefix, w.member);
  if (!items.length) return close();
  if (!force && items.length === 1 && items[0].name === w.prefix && !'fn m s'.includes(items[0].kind)) return close();
  A.items = items; A.idx = 0; A.start = w.s; A.member = w.member; A.open = true;
  render(); place();
}

function accept(i) {
  const it = A.items[i === undefined ? A.idx : i]; if (!it) return close();
  const pos = ta.selectionStart, v = ta.value;
  let end = pos; while (end < v.length && WORD.test(v[end])) end++;
  close();
  if (it.kind === 's') {
    const ls = v.lastIndexOf('\n', A.start - 1) + 1, base = /^[ \t]*/.exec(v.slice(ls, A.start))[0];
    const body = it.snippet.split('\n').map((l, k) => (k ? base : '') + l.replace(/^( {2})+/, (m) => X.ind().repeat(m.length / 2))).join('\n');
    const caret = body.indexOf('$0');
    X.replaceRange(A.start, end, body.replace('$0', ''));
    if (caret >= 0) { const p = A.start + caret; ta.setSelectionRange(p, p); }
    X.onSelect(); return;
  }
  let text = it.name, back = 0;
  const isFn = it.kind === 'fn' || it.kind === 'm';
  if (isFn && v[end] !== '(') {
    const m = /\(([^)]*)\)/.exec(it.sig || '');
    const hasArgs = !m || m[1].trim() !== '';
    text += '()'; back = hasArgs ? 1 : 0;
  }
  X.replaceRange(A.start, end, text);
  const p = A.start + text.length - back; ta.setSelectionRange(p, p);
  X.onSelect();
}
X.completeNow = () => show(true);

ta.addEventListener('keydown', (e) => {
  const mod = e.ctrlKey || e.metaKey;
  if (mod && e.code === 'Space') { e.preventDefault(); show(true); return; }
  if (!A.open) return;
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
    e.preventDefault(); const n = A.items.length;
    A.idx = (A.idx + (e.key === 'ArrowDown' ? 1 : n - 1)) % n; render();
  } else if ((e.key === 'Enter' || e.key === 'Tab') && !mod && !e.shiftKey) { e.preventDefault(); e.stopImmediatePropagation(); accept(); }
  else if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); close(); }
  else if (e.key === 'PageDown' || e.key === 'PageUp') { e.preventDefault(); A.idx = Math.max(0, Math.min(A.items.length - 1, A.idx + (e.key === 'PageDown' ? 6 : -6))); render(); }
  else if (e.key === 'ArrowLeft' || e.key === 'ArrowRight' || e.key === 'Home' || e.key === 'End') close();
}, true);
ac.addEventListener('pointerdown', (e) => { e.preventDefault(); });
ac.addEventListener('click', (e) => { const it = e.target.closest('.it'); if (it) { accept(+it.dataset.i); } });
ta.addEventListener('blur', () => setTimeout(close, 120));
ta.addEventListener('scroll', () => { if (A.open) place(); });
X.closeComplete = close; X.completeOpen = () => A.open;

X.hooks.input.push((e) => {
  const t = e && e.inputType;
  if (t === 'insertText' && e.data && (WORD.test(e.data) || e.data === '.')) show(false);
  else if (t === 'deleteContentBackward' && A.open) show(false);
  else close();
  updateSig();
});
X.hooks.select.push(() => {
  if (A.open) { const pos = ta.selectionStart, w = wordBounds(pos); if (pos !== ta.selectionEnd || w.s !== A.start) close(); }
  updateSig();
});
X.hooks.file.push(() => { close(); updateSig(); });

/* ------------------------------------------------------ signature help -- */
function lookup(name) {
  const r = L.REF_BY_NAME[name]; if (r) return { sig: r.sig, doc: r.doc };
  const s = X.parse().syms.find((x) => x.name === name && x.kind === 'fn');
  if (s) return { sig: s.name + '(' + s.params + ')', doc: 'دالة معرّفة في هذا الملف' };
  const pi = projectIndex().get(name); if (pi && pi.kind === 'fn') return { sig: pi.sig, doc: pi.doc };
  return null;
}
function updateSig() {
  if (!st.set.sigHelp || ta.selectionStart !== ta.selectionEnd || X.inLiteral(ta.selectionStart)) { sig.hidden = true; return; }
  const p = X.parse(), ctx = L.callContext(p.code, p.toks, ta.selectionStart);
  const info = ctx && lookup(ctx.name);
  if (!info) { sig.hidden = true; return; }
  const m = /^([^(]*)\((.*)\)\s*$/.exec(info.sig);
  let html;
  if (m) {
    const parts = []; let depth = 0, cur = '';
    for (const ch of m[2]) { if ('([{'.includes(ch)) depth++; else if (')]}'.includes(ch)) depth--; if ((ch === ',' ) && depth === 0) { parts.push(cur); cur = ''; } else cur += ch; }
    parts.push(cur);
    const variadic = /…|\.\.\./.test(m[2]);
    html = L.esc(m[1]) + '(' + parts.map((x, i) => {
      const on = i === ctx.index || (variadic && i === parts.length - 1 && ctx.index >= i);
      return on ? '<b>' + L.esc(x.trim()) + '</b>' : L.esc(x.trim());
    }).join(', ') + ')';
  } else html = L.esc(info.sig);
  sig.innerHTML = html + (info.doc ? '<span class="d">' + L.esc(info.doc) + '</span>' : '');
  sig.hidden = false;
}
})();
