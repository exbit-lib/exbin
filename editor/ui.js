/* EXBIN editor UI: commands, menu, command palette, shortcuts, panels, settings, projects. */
(() => {
'use strict';
const X = self.EXB, L = X.L, $ = X.$, ta = X.ta, st = X.st, DATA = self.EXB_DATA, esc = L.esc;
const isMac = /Mac|iPhone|iPad/.test(navigator.platform || '');
const MOD = isMac ? '⌘' : 'Ctrl';

/* ----------------------------------------------------------- commands -- */
const C = {};
const cmd = (id, t, k, f) => { C[id] = { id, t, k, f }; };
const cur = X.cur;
function saveCurrent() { X.syncCode(); X.persist(); X.download(cur().name, cur().code); X.toast('نُزّل «' + cur().name + '»'); }
function projectJSON() { X.syncCode(); return JSON.stringify({ format: 'exbin-project', version: 1, active: cur().name, files: st.files.map((f) => ({ name: f.name, code: f.code })) }, null, 1); }

cmd('run', 'تشغيل', MOD + '+Enter', () => X.runCode('run'));
cmd('stop', 'إيقاف التنفيذ', null, () => X.stopCode());
cmd('disasm', 'عرض الـ bytecode', null, () => X.runCode('disasm'));
cmd('new', 'ملف جديد…', 'Alt+N', () => X.promptNew());
cmd('open', 'فتح ملف…', MOD + '+O', () => $('#fileIn').click());
cmd('save', 'تنزيل الملف الحالي', MOD + '+S', saveCurrent);
cmd('zip', 'تنزيل كل الملفات (ZIP)', null, () => { X.syncCode(); X.download('exbin-project.zip', makeZip(st.files.map((f) => ({ name: f.name, data: new TextEncoder().encode(f.code) }))), 'application/zip'); X.toast('نُزّلت ' + st.files.length + ' ملفات'); });
cmd('project', 'تصدير المشروع (.exbproj)', null, () => { X.download('project.exbproj', projectJSON(), 'application/json'); });
cmd('rename', 'إعادة تسمية الملف', null, () => X.renameFile(st.active));
cmd('dupfile', 'تكرار الملف', null, () => X.duplicateFile(st.active));
cmd('closetab', 'إغلاق الملف', null, () => X.closeFile(st.active));
cmd('reopen', 'إعادة فتح ملف مُغلق', MOD + '+Shift+T', () => X.reopenClosed());
cmd('find', 'بحث', MOD + '+F', () => X.openFind(false));
cmd('replace', 'بحث واستبدال', MOD + '+H', () => X.openFind(true));
cmd('goto', 'اذهب إلى سطر…', MOD + '+G', () => openPalette(':'));
cmd('symbols', 'اذهب إلى رمز (دالة/متغير)…', MOD + '+Shift+O', () => openPalette('@'));
cmd('format', 'تنسيق الكود', 'Shift+Alt+F', () => X.formatDoc());
cmd('comment', 'تعليق / إلغاء تعليق', MOD + '+/', () => X.edit.comment());
cmd('dupline', 'تكرار السطر', MOD + '+D', () => X.edit.dup(false));
cmd('delline', 'حذف السطر', MOD + '+Shift+K', () => X.edit.del());
cmd('up', 'نقل السطر لأعلى', 'Alt+↑', () => X.edit.move(-1));
cmd('down', 'نقل السطر لأسفل', 'Alt+↓', () => X.edit.move(1));
cmd('complete', 'إظهار الاقتراحات', MOD + '+Space', () => X.completeNow());
cmd('outline', 'مخطط الرموز', null, () => toggleOutline());
cmd('theme', 'تبديل الوضع (فاتح / داكن)', null, () => { st.theme = document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'; X.applyTheme(); X.persist(); });
cmd('settings', 'الإعدادات', MOD + '+,', () => openSettings());
cmd('stdin', 'الإدخال القياسي (stdin)', null, () => toggleStdin());
cmd('clear', 'مسح الناتج', null, () => clearOut());
cmd('problems', 'عرض المشاكل', null, () => X.showPane('problems'));
cmd('ref', 'مرجع الدوال', null, () => { X.showPane('ref'); $('#refSearch').focus(); });
cmd('shortcuts', 'اختصارات لوحة المفاتيح', null, () => shortcuts());
cmd('about', 'حول المحرر', null, () => about());
let deferredInstall = null;
cmd('install', 'تثبيت التطبيق', null, () => { if (deferredInstall) { deferredInstall.prompt(); deferredInstall = null; } });
window.addEventListener('beforeinstallprompt', (e) => { e.preventDefault(); deferredInstall = e; buildMenu(); });
window.addEventListener('appinstalled', () => { deferredInstall = null; buildMenu(); });

/* --------------------------------------------------------------- menu -- */
const menu = $('#menu');
const MENU = [
  ['run', 'disasm'],
  ['new', 'open', 'save', 'zip', 'project', 'reopen'],
  ['find', 'replace', 'goto', 'symbols', 'format'],
  ['outline', 'ref', 'settings', 'theme'],
  'EX',
  'FONT',
  ['shortcuts', 'install', 'about'],
];
function buildMenu() {
  menu.textContent = '';
  const btn = (c) => { const b = document.createElement('button'); b.dataset.cmd = c.id; b.innerHTML = '<span>' + esc(c.t) + '</span>' + (c.k ? '<kbd>' + esc(c.k) + '</kbd>' : ''); return b; };
  MENU.forEach((g, gi) => {
    if (gi) menu.appendChild(document.createElement('hr'));
    if (g === 'EX') {
      const t = document.createElement('div'); t.className = 'menu-title'; t.textContent = 'أمثلة'; menu.appendChild(t);
      for (const e of DATA.examples) { const b = document.createElement('button'); b.dataset.ex = e.name; b.textContent = e.title; menu.appendChild(b); }
    } else if (g === 'FONT') {
      const r = document.createElement('div'); r.className = 'row';
      r.innerHTML = '<span>حجم الخط</span><span><button data-font="-1">A−</button><button data-font="1">A+</button></span>'; menu.appendChild(r);
    } else for (const id of g) { if (id === 'install' && !deferredInstall) continue; menu.appendChild(btn(C[id])); }
  });
}
function closeMenu() { menu.hidden = true; }
$('#btnMenu').addEventListener('click', (e) => { e.stopPropagation(); closeCtx(); menu.hidden = !menu.hidden; });
document.addEventListener('click', (e) => { if (!menu.hidden && !menu.contains(e.target)) closeMenu(); if (!ctx.hidden && !ctx.contains(e.target)) closeCtx(); });
menu.addEventListener('click', (e) => {
  const b = e.target.closest('button'); if (!b) return;
  if (b.dataset.font) { st.fs = Math.max(11, Math.min(26, st.fs + +b.dataset.font)); X.applyFont(); X.persist(); return; }
  if (b.dataset.ex) { const x = DATA.examples.find((z) => z.name === b.dataset.ex); X.addFile(x.name, x.code); closeMenu(); return; }
  if (b.dataset.cmd) { closeMenu(); C[b.dataset.cmd].f(); }
});
$('#btnPalette').addEventListener('click', () => openPalette(''));
$('#btnOutline').addEventListener('click', () => toggleOutline());
$('#btnRun').addEventListener('click', () => X.runCode('run'));
$('#btnDisasm').addEventListener('click', () => X.runCode('disasm'));
$('#btnStop').addEventListener('click', () => X.stopCode());
$('#btnAdd').addEventListener('click', () => X.promptNew());

/* ---------------------------------------------------------- tab events -- */
const tabList = $('#tabList'), ctx = $('#tabCtx');
tabList.addEventListener('click', (e) => {
  const c = e.target.closest('[data-close]'); if (c) return X.closeFile(+c.dataset.close);
  const t = e.target.closest('.tab'); if (t) X.openFile(+t.dataset.id);
});
tabList.addEventListener('dblclick', (e) => { const t = e.target.closest('.tab'); if (t) X.renameFile(+t.dataset.id); });
tabList.addEventListener('auxclick', (e) => { if (e.button === 1) { const t = e.target.closest('.tab'); if (t) X.closeFile(+t.dataset.id); } });
function closeCtx() { ctx.hidden = true; }
function openCtx(id, x, y) {
  const f = st.files.find((z) => z.id === id); if (!f) return;
  const items = [['إعادة تسمية', () => X.renameFile(id)], ['تكرار', () => X.duplicateFile(id)],
    ['تنزيل', () => { if (id === st.active) X.syncCode(); X.download(f.name, f.code); }],
    ['إغلاق', () => X.closeFile(id)],
    ['إغلاق الملفات الأخرى', () => { for (const g of st.files.filter((z) => z.id !== id)) X.closeFile(g.id); X.openFile(id); }]];
  ctx.textContent = '';
  for (const [t, fn] of items) { const b = document.createElement('button'); b.textContent = t; b.onclick = () => { closeCtx(); fn(); }; ctx.appendChild(b); }
  ctx.style.position = 'fixed'; ctx.hidden = false;
  ctx.style.left = Math.max(4, Math.min(x, innerWidth - ctx.offsetWidth - 4)) + 'px';
  ctx.style.top = Math.min(y, innerHeight - ctx.offsetHeight - 4) + 'px';
}
tabList.addEventListener('contextmenu', (e) => { const t = e.target.closest('.tab'); if (!t) return; e.preventDefault(); openCtx(+t.dataset.id, e.clientX, e.clientY); });
(() => { // long-press (touch) opens the tab menu
  let timer = 0;
  tabList.addEventListener('pointerdown', (e) => {
    if (e.pointerType === 'mouse') return;
    const t = e.target.closest('.tab'); if (!t || e.target.closest('.x')) return;
    const x = e.clientX, y = e.clientY; timer = setTimeout(() => openCtx(+t.dataset.id, x, y), 600);
  });
  for (const ev of ['pointerup', 'pointerleave', 'pointercancel', 'scroll']) tabList.addEventListener(ev, () => clearTimeout(timer));
})();

/* ------------------------------------------------------ output panes -- */
const panes = { out: $('#out'), problems: $('#problems'), ref: $('#ref') };
X.showPane = (name) => {
  for (const [k, el] of Object.entries(panes)) el.hidden = k !== name;
  for (const b of document.querySelectorAll('#outTabs button')) b.classList.toggle('on', b.dataset.pane === name);
  if (name === 'problems') renderProblems();
};
$('#outTabs').addEventListener('click', (e) => { const b = e.target.closest('button'); if (b) X.showPane(b.dataset.pane); });
$('#stProb').addEventListener('click', () => { X.showPane('problems'); $('#outPane').scrollIntoView({ block: 'nearest' }); });
function clearOut() { X.out.textContent = ''; $('#outMeta').textContent = ''; X.showPane('out'); }
$('#btnClear').addEventListener('click', clearOut);
$('#btnCopy').addEventListener('click', async () => {
  const pane = !panes.problems.hidden ? panes.problems : panes.out, text = pane.innerText;
  try { await navigator.clipboard.writeText(text); } catch (_) {
    const t = document.createElement('textarea'); t.value = text; document.body.appendChild(t); t.select(); document.execCommand('copy'); t.remove();
  }
  X.toast('نُسخ الناتج');
});
function toggleStdin(force) {
  const w = $('#stdinWrap'); w.hidden = force === undefined ? !w.hidden : !force;
  $('#btnStdin').classList.toggle('on', !w.hidden);
  if (!w.hidden) $('#stdin').focus();
}
$('#btnStdin').addEventListener('click', () => toggleStdin());
$('#stdin').value = st.stdin; if (st.stdin) toggleStdin(true);
$('#stdin').addEventListener('input', (e) => { st.stdin = e.target.value; X.persist(); });

/* problems list */
function renderProblems() {
  const box = panes.problems; if (box.hidden) return;
  box.textContent = '';
  const list = X.problems || [];
  if (!list.length) { const d = document.createElement('div'); d.className = 'empty'; d.textContent = 'لا توجد مشاكل ✓'; box.appendChild(d); return; }
  const w = document.createElement('div'); w.className = 'plist';
  for (const p of list) {
    const b = document.createElement('button');
    b.innerHTML = '<span class="sev">✗</span><span class="msg">' + esc(p.msg) + '</span><span class="loc">' + esc(p.file) + ':' + p.line + '</span>';
    b.onclick = () => { if (p.fid !== st.active) X.openFile(p.fid); X.gotoLine(p.line, true); };
    w.appendChild(b);
  }
  box.appendChild(w);
}
X.hooks.render.push((w) => { if (w === 'problems') { renderProblems(); if (!$('#outlineList').closest('#outline').hidden) renderOutline(); } });

/* reference */
(() => {
  const list = $('#refList'), q = $('#refSearch');
  const all = [...L.REF, ...L.METHODS.map((m) => ({ ...m, cat: 'طرق: ' + m.kind }))];
  function draw() {
    const s = q.value.trim().toLowerCase(); list.textContent = '';
    let lastCat = '', n = 0;
    for (const r of all) {
      if (s && !(r.name.toLowerCase().includes(s) || r.doc.includes(s) || r.cat.includes(s) || r.sig.toLowerCase().includes(s))) continue;
      if (r.cat !== lastCat) { const c = document.createElement('div'); c.className = 'cat'; c.textContent = r.cat; list.appendChild(c); lastCat = r.cat; }
      const b = document.createElement('button'); b.className = 'r'; b.dataset.name = r.name; b.dataset.kind = r.kind;
      b.innerHTML = '<code>' + esc(r.sig) + '</code><span>' + esc(r.doc) + '</span>'; list.appendChild(b); n++;
    }
    if (!n) { const d = document.createElement('div'); d.className = 'empty'; d.textContent = 'لا نتائج'; list.appendChild(d); }
  }
  q.addEventListener('input', draw); draw();
  list.addEventListener('click', (e) => {
    const b = e.target.closest('.r'); if (!b) return;
    const name = b.dataset.name, r = (b.dataset.kind === 'method' ? L.METHODS : L.REF).find((x) => x.name === name);
    const args = /\(([^)]*)\)/.exec(r.sig); const empty = args && !args[1].trim();
    ta.focus(); X.ins(b.dataset.kind === 'method' ? name + '()' : name + '()');
    if (!empty) { const p = ta.selectionStart - 1; ta.setSelectionRange(p, p); }
    X.onSelect();
  });
})();

/* ------------------------------------------------------------ outline -- */
const outlineEl = $('#outline');
function renderOutline() {
  const box = $('#outlineList'); box.textContent = '';
  const syms = X.parse().syms;
  if (!syms.length) { const d = document.createElement('div'); d.className = 'empty'; d.textContent = 'لا توجد دوال أو متغيرات عامة بعد.'; box.appendChild(d); return; }
  for (const s of syms) {
    const b = document.createElement('button');
    b.style.paddingInlineStart = (8 + Math.min(s.depth, 4) * 12) + 'px';
    b.innerHTML = '<span style="color:var(--' + (s.kind === 'fn' ? 'c-fn' : 'c-num') + ')">' + (s.kind === 'fn' ? 'ƒ' : 'x') + '</span><span>' + esc(s.name) + (s.kind === 'fn' ? '(' + esc(s.params) + ')' : '') + '</span><span class="ln">' + s.line + '</span>';
    b.onclick = () => { X.revealRange(s.pos, s.pos + s.name.length); if (matchMedia('(max-width: 700px)').matches) toggleOutline(false); };
    box.appendChild(b);
  }
}
function toggleOutline(force) {
  outlineEl.hidden = force === undefined ? !outlineEl.hidden : !force;
  if (!outlineEl.hidden) renderOutline();
}
$('#oClose').addEventListener('click', () => toggleOutline(false));
let oTimer = 0;
X.hooks.input.push(() => { if (!outlineEl.hidden) { clearTimeout(oTimer); oTimer = setTimeout(renderOutline, 250); } });
X.hooks.file.push(() => { if (!outlineEl.hidden) renderOutline(); });

/* ------------------------------------------------------------ palette -- */
const pal = $('#palette'), pIn = $('#pIn'), pList = $('#pList');
let pItems = [], pIdx = 0;
function fuzzy(q, text) { // subsequence match → {score, idx}
  if (!q) return { score: 0, idx: [] };
  const a = q.toLowerCase(), b = text.toLowerCase(), idx = []; let j = 0, score = 0, prev = -2;
  for (let i = 0; i < b.length && j < a.length; i++) if (b[i] === a[j]) { idx.push(i); score += (i === prev + 1 ? 5 : 1) + (i === 0 || ' _-.:'.includes(b[i - 1]) ? 4 : 0); prev = i; j++; }
  return j === a.length ? { score: score - text.length * 0.02, idx } : null;
}
function hiText(text, idx) {
  if (!idx.length) return esc(text);
  let o = '', set = new Set(idx);
  for (let i = 0; i < text.length; i++) o += set.has(i) ? '<mark>' + esc(text[i]) + '</mark>' : esc(text[i]);
  return o;
}
function palItems(q) {
  if (q[0] === ':') {
    const n = parseInt(q.slice(1), 10), max = X.parse().starts.length;
    return [{ t: isNaN(n) ? 'اكتب رقم السطر (1–' + max + ')' : 'اذهب إلى السطر ' + Math.max(1, Math.min(n, max)), run: isNaN(n) ? null : () => X.gotoLine(Math.max(1, Math.min(n, max)), true) }];
  }
  const out = [];
  if (q[0] === '@') {
    const s = q.slice(1);
    for (const y of X.parse().syms) { const f = fuzzy(s, y.name); if (f) out.push({ t: y.name, hint: y.kind === 'fn' ? 'ƒ(' + y.params + ')' : 'let', small: ':' + y.line, score: f.score, idx: f.idx, run: () => X.revealRange(y.pos, y.pos + y.name.length) }); }
    return out.sort((a, b) => b.score - a.score);
  }
  for (const c of Object.values(C)) { if (c.id === 'install' && !deferredInstall) continue; const f = fuzzy(q, c.t); if (f) out.push({ t: c.t, small: c.k || '', score: f.score + 1, idx: f.idx, run: c.f }); }
  for (const f of st.files) { const m = fuzzy(q, f.name); if (m) out.push({ t: f.name, hint: 'ملف', score: m.score + (f.id === st.active ? -1 : 2), idx: m.idx, run: () => X.openFile(f.id) }); }
  for (const e of DATA.examples) { const m = fuzzy(q, e.title); if (m) out.push({ t: e.title, hint: 'مثال', score: m.score, idx: m.idx, run: () => X.addFile(e.name, e.code) }); }
  return q ? out.sort((a, b) => b.score - a.score) : out;
}
function drawPalette() {
  pItems = palItems(pIn.value).slice(0, 60); pIdx = Math.min(pIdx, Math.max(0, pItems.length - 1));
  pList.textContent = '';
  if (!pItems.length) { const d = document.createElement('div'); d.className = 'empty'; d.textContent = 'لا نتائج'; pList.appendChild(d); return; }
  pItems.forEach((it, i) => {
    const b = document.createElement('button'); b.className = 'p' + (i === pIdx ? ' on' : ''); b.dataset.i = i; b.tabIndex = -1;
    b.innerHTML = '<span>' + hiText(it.t, it.idx || []) + '</span>' + (it.hint ? '<em>' + esc(it.hint) + '</em>' : '') + (it.small ? '<small>' + esc(it.small) + '</small>' : '');
    pList.appendChild(b);
  });
  const on = pList.querySelector('.on'); if (on) on.scrollIntoView({ block: 'nearest' });
}
function openPalette(prefix) { pal.hidden = false; pIn.value = prefix || ''; pIdx = 0; drawPalette(); pIn.focus(); }
function closePalette() { if (pal.hidden) return; pal.hidden = true; ta.focus(); }
function runPalette(i) { const it = pItems[i === undefined ? pIdx : i]; if (!it || !it.run) return; closePalette(); setTimeout(it.run, 0); }
pIn.addEventListener('input', () => { pIdx = 0; drawPalette(); });
pIn.addEventListener('keydown', (e) => {
  if (e.key === 'ArrowDown') { e.preventDefault(); pIdx = Math.min(pItems.length - 1, pIdx + 1); drawPalette(); }
  else if (e.key === 'ArrowUp') { e.preventDefault(); pIdx = Math.max(0, pIdx - 1); drawPalette(); }
  else if (e.key === 'Enter') { e.preventDefault(); runPalette(); }
  else if (e.key === 'Escape') { e.preventDefault(); closePalette(); }
});
pList.addEventListener('click', (e) => { const b = e.target.closest('.p'); if (b) runPalette(+b.dataset.i); });
pal.addEventListener('mousedown', (e) => { if (e.target === pal) closePalette(); });

/* ------------------------------------------------------------ dialogs -- */
function about() {
  $('#dlgBody').innerHTML = '<h3>محرر EXBIN</h3><p>يعمل بالكامل على جهازك وبدون إنترنت: محرك EXBIN مُترجَم إلى WebAssembly ويعمل في Worker منفصل.</p>' +
    '<p style="color:var(--muted)">اضغط <kbd>F1</kbd> للوحة الأوامر. الملفات المفتوحة تعمل مع <code>require("اسم")</code>، والمكتبتان <code>pratt</code> و<code>pretty</code> جاهزتان.</p>' +
    '<p style="color:var(--muted)">الفحص المباشر يترجم الكود في الخلفية ويُظهر أخطاء الصياغة أثناء الكتابة.</p>';
  $('#dlg').showModal();
}
function shortcuts() {
  const rows = [['F1 / ' + MOD + '+Shift+P', 'لوحة الأوامر'], [MOD + '+Enter / F5', 'تشغيل'], [MOD + '+F / ' + MOD + '+H', 'بحث / استبدال'], [MOD + '+G', 'اذهب إلى سطر'],
    [MOD + '+Shift+O', 'اذهب إلى رمز'], [MOD + '+Space', 'اقتراحات'], [MOD + '+/', 'تعليق'], [MOD + '+D', 'تكرار السطر'], [MOD + '+Shift+K', 'حذف السطر'],
    ['Alt+↑ / Alt+↓', 'نقل السطر'], ['Shift+Alt+F', 'تنسيق الكود'], ['Tab / Shift+Tab', 'إزاحة / إلغاء إزاحة'], [MOD + '+S', 'تنزيل الملف'], [MOD + '+Shift+T', 'إعادة فتح ملف مُغلق'], ['Alt+N', 'ملف جديد']];
  $('#dlgBody').innerHTML = '<h3>اختصارات لوحة المفاتيح</h3><table class="keys">' + rows.map((r) => '<tr><td><kbd>' + esc(r[0]) + '</kbd></td><td>' + esc(r[1]) + '</td></tr>').join('') + '</table>';
  $('#dlg').showModal();
}
function openSettings() {
  const S = st.set, b = $('#setBody');
  const chk = (k, t) => '<label class="srow"><span>' + t + '</span><input type="checkbox" data-k="' + k + '"' + (S[k] ? ' checked' : '') + '></label>';
  b.innerHTML = '<h3>الإعدادات</h3>' +
    '<div class="srow"><span>حجم الخط</span><input type="number" min="11" max="26" data-f="fs" value="' + st.fs + '"></div>' +
    '<div class="srow"><span>عرض الإزاحة</span><select data-f="tab"><option value="2"' + (S.tab === 2 ? ' selected' : '') + '>2 مسافات</option><option value="4"' + (S.tab === 4 ? ' selected' : '') + '>4 مسافات</option></select></div>' +
    '<div class="srow"><span>المظهر</span><select data-f="theme"><option value="">تلقائي</option><option value="light"' + (st.theme === 'light' ? ' selected' : '') + '>فاتح</option><option value="dark"' + (st.theme === 'dark' ? ' selected' : '') + '>داكن</option></select></div>' +
    chk('autoClose', 'إغلاق الأقواس وعلامات الاقتباس تلقائياً') + chk('live', 'فحص الأخطاء أثناء الكتابة') + chk('suggest', 'اقتراحات الإكمال التلقائي') +
    chk('sigHelp', 'عرض توقيع الدالة أثناء الاستدعاء') + chk('curLine', 'تمييز السطر الحالي') + chk('occ', 'تمييز تكرارات الكلمة') +
    '<div class="srow"><span style="color:var(--warn)">مسح كل بيانات المحرر من هذا الجهاز</span><button type="button" id="wipe">مسح</button></div>';
  $('#setDlg').showModal();
}
$('#setBody').addEventListener('change', (e) => {
  const t = e.target;
  if (t.dataset.k) { st.set[t.dataset.k] = t.checked; if (t.dataset.k === 'live') X.setLive(t.checked); X.closeComplete(); X.renderAll(); }
  else if (t.dataset.f === 'fs') { st.fs = Math.max(11, Math.min(26, +t.value || 15)); X.applyFont(); }
  else if (t.dataset.f === 'tab') { st.set.tab = +t.value; X.applyFont(); }
  else if (t.dataset.f === 'theme') { st.theme = t.value || null; X.applyTheme(); }
  X.persist();
});
$('#setBody').addEventListener('click', (e) => {
  if (e.target.id === 'wipe' && confirm('سيُحذف كل شيء (الملفات والإعدادات) من هذا الجهاز. متابعة؟')) {
    try { localStorage.removeItem('exbin.editor.v1'); } catch (_) {}
    window.onbeforeunload = null; location.reload();
  }
});

/* ---------------------------------------------------- files & projects -- */
const crcT = (() => { const t = new Uint32Array(256); for (let n = 0; n < 256; n++) { let c = n; for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1; t[n] = c >>> 0; } return t; })();
const crc32 = (d) => { let c = 0xFFFFFFFF; for (let i = 0; i < d.length; i++) c = crcT[(c ^ d[i]) & 255] ^ (c >>> 8); return (c ^ 0xFFFFFFFF) >>> 0; };
function makeZip(files) { // "store" method, UTF-8 names
  const enc = new TextEncoder(), parts = [], cd = []; let off = 0;
  const d = new Date(), time = (d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1), date = ((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate();
  for (const f of files) {
    const name = enc.encode(f.name), crc = crc32(f.data), n = f.data.length;
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(6, 0x0800, true); h.setUint16(8, 0, true); h.setUint16(10, time, true); h.setUint16(12, date, true);
    h.setUint32(14, crc, true); h.setUint32(18, n, true); h.setUint32(22, n, true); h.setUint16(26, name.length, true); h.setUint16(28, 0, true);
    parts.push(new Uint8Array(h.buffer), name, f.data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(8, 0x0800, true); c.setUint16(10, 0, true); c.setUint16(12, time, true); c.setUint16(14, date, true);
    c.setUint32(16, crc, true); c.setUint32(20, n, true); c.setUint32(24, n, true); c.setUint16(28, name.length, true); c.setUint32(42, off, true);
    cd.push(new Uint8Array(c.buffer), name);
    off += 30 + name.length + n;
  }
  const cdSize = cd.reduce((a, p) => a + p.length, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true); e.setUint32(12, cdSize, true); e.setUint32(16, off, true);
  return new Blob([...parts, ...cd, new Uint8Array(e.buffer)], { type: 'application/zip' });
}
X.makeZip = makeZip;
async function importFiles(fl) {
  let n = 0;
  for (const f of fl) {
    if (/\.exbc$/i.test(f.name)) { X.toast('ملفات .exbc الثنائية غير مدعومة في المحرر'); continue; }
    const text = await f.text();
    if (/\.(exbproj|json)$/i.test(f.name)) {
      let j = null; try { j = JSON.parse(text); } catch (_) {}
      if (j && j.format === 'exbin-project' && Array.isArray(j.files)) { for (const x of j.files) if (x && typeof x.name === 'string') { X.addFile(x.name, String(x.code || '')); n++; } continue; }
    }
    X.addFile(f.name, text); n++;
  }
  if (n > 1) X.toast('فُتح ' + n + ' ملفات');
}
$('#fileIn').addEventListener('change', async (e) => { await importFiles([...e.target.files]); e.target.value = ''; });
(() => { // drag & drop
  const d = $('#drop'); let depth = 0;
  window.addEventListener('dragenter', (e) => { if ([...(e.dataTransfer?.types || [])].includes('Files')) { depth++; d.hidden = false; e.preventDefault(); } });
  window.addEventListener('dragover', (e) => { if (!d.hidden) e.preventDefault(); });
  window.addEventListener('dragleave', () => { depth = Math.max(0, depth - 1); if (!depth) d.hidden = true; });
  window.addEventListener('drop', async (e) => { if (d.hidden) return; e.preventDefault(); depth = 0; d.hidden = true; await importFiles([...e.dataTransfer.files]); });
})();

/* -------------------------------------------------------- symbols bar -- */
(() => {
  const bar = $('#keys');
  const keys = [['↹', 'tab'], ['↶', 'undo'], ['↷', 'redo'], ['⋯', 'complete'], '{', '}', '(', ')', '[', ']', '"', "'", '=', '+', '-', '*', '/', '%', ';', ':', '.', ',', '!', '<', '>', '&', '|', '_'];
  for (const k of keys) {
    const b = document.createElement('button'); b.type = 'button';
    b.textContent = Array.isArray(k) ? k[0] : k; b.dataset.k = Array.isArray(k) ? k[1] : k;
    if (Array.isArray(k)) b.setAttribute('aria-label', k[1]);
    bar.appendChild(b);
  }
  const keep = (e) => e.preventDefault();
  bar.addEventListener('pointerdown', keep); bar.addEventListener('mousedown', keep);
  bar.addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    const k = b.dataset.k; ta.focus();
    if (k === 'tab') X.edit.indent(false);
    else if (k === 'undo') document.execCommand('undo');
    else if (k === 'redo') document.execCommand('redo');
    else if (k === 'complete') X.completeNow();
    else if (!['(', '[', '{', '"', "'", ')', ']', '}'].includes(k)) X.ins(k);
    else { const ev = new InputEvent('beforeinput', { inputType: 'insertText', data: k, cancelable: true, bubbles: true }); if (ta.dispatchEvent(ev)) X.ins(k); }
  });
})();

/* ---------------------------------------------------------- shortcuts -- */
ta.addEventListener('keydown', (e) => {
  const mod = e.ctrlKey || e.metaKey, k = e.key;
  if (k === 'Tab' && !mod && !e.altKey) { e.preventDefault(); X.edit.indent(e.shiftKey); }
  else if (mod && !e.shiftKey && k === '/') { e.preventDefault(); X.edit.comment(); }
  else if (mod && !e.shiftKey && !e.altKey && (k === 'd' || k === 'D')) { e.preventDefault(); X.edit.dup(false); }
  else if (mod && e.shiftKey && (k === 'k' || k === 'K')) { e.preventDefault(); X.edit.del(); }
  else if (e.altKey && !mod && k === 'ArrowUp' && !e.shiftKey) { e.preventDefault(); X.edit.move(-1); }
  else if (e.altKey && !mod && k === 'ArrowDown' && !e.shiftKey) { e.preventDefault(); X.edit.move(1); }
  else if (e.altKey && e.shiftKey && k === 'ArrowDown') { e.preventDefault(); X.edit.dup(false); }
  else if (e.altKey && e.shiftKey && k === 'ArrowUp') { e.preventDefault(); X.edit.dup(true); }
  else if (mod && k === ']') { e.preventDefault(); X.edit.indent(false); }
  else if (mod && k === '[') { e.preventDefault(); X.edit.indent(true); }
  else if (k === 'Escape' && !X.completeOpen()) { X.closeFind(); }
});
document.addEventListener('keydown', (e) => {
  const mod = e.ctrlKey || e.metaKey, k = e.key, lk = k.length === 1 ? k.toLowerCase() : k;
  if (!pal.hidden) return;
  const stop = () => { e.preventDefault(); e.stopPropagation(); };
  if (k === 'F1' || (mod && e.shiftKey && lk === 'p')) { stop(); openPalette(''); }
  else if (mod && e.shiftKey && lk === 'o') { stop(); openPalette('@'); }
  else if (mod && e.shiftKey && lk === 't') { stop(); X.reopenClosed(); }
  else if (mod && !e.shiftKey && lk === 'g') { stop(); openPalette(':'); }
  else if (mod && !e.shiftKey && lk === 'f') { stop(); X.openFind(false); }
  else if (mod && !e.shiftKey && lk === 'h') { stop(); X.openFind(true); }
  else if (mod && !e.shiftKey && lk === 's') { stop(); saveCurrent(); }
  else if (mod && !e.shiftKey && lk === 'o') { stop(); $('#fileIn').click(); }
  else if (mod && k === ',') { stop(); openSettings(); }
  else if (mod && k === 'Enter') { stop(); X.runCode('run'); }
  else if (k === 'F5') { stop(); X.runCode('run'); }
  else if (e.altKey && e.shiftKey && lk === 'f') { stop(); X.formatDoc(); }
  else if (e.altKey && !mod && lk === 'n') { stop(); X.promptNew(); }
  else if (k === 'Escape') {
    if (!menu.hidden) closeMenu(); else if (!ctx.hidden) closeCtx(); else if (!outlineEl.hidden && document.activeElement !== ta) toggleOutline(false);
  }
}, true);

/* ----------------------------------------------------------- splitter -- */
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

/* --------------------------------------------------------------- boot -- */
buildMenu();
X.start();
if ('serviceWorker' in navigator && /^https?:$/.test(location.protocol)) {
  navigator.serviceWorker.register('sw.js').then((reg) => {
    reg.addEventListener('updatefound', () => {
      const w = reg.installing;
      if (w) w.addEventListener('statechange', () => { if (w.state === 'installed' && navigator.serviceWorker.controller) X.toast('يتوفر تحديث — أعد تحميل الصفحة'); });
    });
  }).catch(() => {});
}
})();
