/* EXBIN language support: tokenizer, reference data, formatter, symbols, snippets.
   Pure functions, no DOM — shared by the UI (app.js). */
(function (root) {
'use strict';

/* ---------------------------------------------------------------- lexicon -- */
const KEYWORDS = new Set('and break continue else fn for if in let not or return while'.split(' '));
const CONSTS = new Set('true false nil PI E INF EXBIN_VERSION args'.split(' '));

/* name | signature | description (Arabic) | category.  Arities verified against src/natives.c / toolkit.c */
const REF_SRC = `
print|print(قيم…)|يطبع القيم مفصولة بمسافة ثم سطراً جديداً|إدخال/إخراج
write|write(قيم…)|يطبع دون سطر جديد|إدخال/إخراج
input|input([سؤال])|يقرأ سطراً من الإدخال القياسي (stdin) — يُعيد nil عند النهاية|إدخال/إخراج
read_file|read_file(مسار)|يقرأ ملفاً كنص (نظام ملفات مؤقت في المتصفح)|إدخال/إخراج
read_bytes|read_bytes(مسار)|يقرأ ملفاً كـ bytes|إدخال/إخراج
write_file|write_file(مسار, بيانات)|يكتب ملفاً|إدخال/إخراج
append_file|append_file(مسار, بيانات)|يضيف إلى ملف|إدخال/إخراج
file_exists|file_exists(مسار)|هل الملف موجود؟|إدخال/إخراج
env|env(اسم)|قيمة متغير بيئة|إدخال/إخراج
exit|exit([رمز])|ينهي البرنامج|إدخال/إخراج
type|type(قيمة)|اسم نوع القيمة|أنواع وتحويل
str|str(قيمة)|تحويل إلى نص|أنواع وتحويل
repr|repr(قيمة)|تمثيل نصي للتنقيح (النصوص بين علامات اقتباس)|أنواع وتحويل
num|num(قيمة)|تحويل إلى رقم|أنواع وتحويل
int|int(قيمة)|تحويل إلى عدد صحيح|أنواع وتحويل
list|list(قيمة)|تحويل (نطاق، نص، خريطة…) إلى قائمة|أنواع وتحويل
bytes|bytes(قيمة)|إنشاء bytes|أنواع وتحويل
format|format(نمط, قيم…)|تنسيق نص: %s %d %f %x %r مع عرض/دقة بالحروف|أنواع وتحويل
len|len(x)|الطول (نص، قائمة، خريطة، bytes، نطاق)|حاويات
push|push(قائمة, قيم…)|يضيف عناصر في النهاية|حاويات
pop|pop(قائمة[, فهرس])|يزيل ويُعيد عنصراً|حاويات
insert|insert(قائمة, فهرس, قيمة)|يُدرج عنصراً في موضع|حاويات
remove|remove(قائمة, فهرس)|يزيل العنصر في الفهرس|حاويات
delete|delete(خريطة, مفتاح)|يحذف مفتاحاً من خريطة|حاويات
keys|keys(خريطة)|قائمة المفاتيح|حاويات
values|values(خريطة)|قائمة القيم|حاويات
items|items(خريطة)|قائمة أزواج [مفتاح, قيمة]|حاويات
has|has(حاوية, عنصر)|هل يوجد المفتاح/العنصر؟|حاويات
find|find(حاوية, قيمة[, من])|فهرس أول تطابق (أو -1/nil)|حاويات
copy|copy(حاوية)|نسخة سطحية|حاويات
slice|slice(x, من[, إلى])|شريحة من نص/قائمة/bytes|حاويات
join|join(قائمة[, فاصل])|يدمج عناصر القائمة في نص|حاويات
reverse|reverse(x)|يعكس نصاً أو قائمة|حاويات
sort|sort(قائمة[, مقارنة])|يرتّب القائمة (دالة مقارنة اختيارية)|حاويات
map|map(قائمة, دالة)|يطبّق دالة على كل عنصر|حاويات
filter|filter(قائمة, دالة)|يُبقي العناصر التي تحقق الشرط|حاويات
reduce|reduce(قائمة, دالة[, بداية])|يختزل القائمة إلى قيمة|حاويات
each|each(قائمة, دالة)|ينفّذ دالة لكل عنصر|حاويات
any|any(قائمة[, دالة])|هل يحقق أي عنصر الشرط؟|حاويات
all|all(قائمة[, دالة])|هل يحقق كل العناصر الشرط؟|حاويات
sum|sum(قائمة)|مجموع الأرقام|حاويات
zip|zip(أ, ب)|يزاوج قائمتين|حاويات
enumerate|enumerate(قائمة)|أزواج [فهرس, عنصر]|حاويات
range|range(نهاية) / range(بداية, نهاية[, خطوة])|نطاق أرقام|حاويات
split|split(نص[, فاصل])|يقسّم النص|نصوص
lines|lines(نص)|يقسّم إلى أسطر|نصوص
chars|chars(نص)|قائمة حروف UTF-8|نصوص
replace|replace(نص, قديم, جديد[, عدد])|استبدال داخل نص|نصوص
upper|upper(نص)|أحرف كبيرة|نصوص
lower|lower(نص)|أحرف صغيرة|نصوص
trim|trim(نص)|يحذف الفراغات من الطرفين|نصوص
trim_start|trim_start(نص)|يحذف الفراغات من البداية|نصوص
trim_end|trim_end(نص)|يحذف الفراغات من النهاية|نصوص
starts_with|starts_with(نص, بادئة)|هل يبدأ بـ؟|نصوص
ends_with|ends_with(نص, لاحقة)|هل ينتهي بـ؟|نصوص
repeat|repeat(نص, عدد)|يكرر النص|نصوص
pad_left|pad_left(نص, عرض[, حشو])|حشو من اليسار (بالحروف)|نصوص
pad_right|pad_right(نص, عرض[, حشو])|حشو من اليمين (بالحروف)|نصوص
substr|substr(نص, بداية[, طول])|جزء من نص|نصوص
chr|chr(رمز)|الحرف من رمز Unicode|نصوص
ord|ord(حرف)|رمز Unicode للحرف|نصوص
utf8_len|utf8_len(نص)|عدد حروف UTF-8|نصوص
to_string|to_string(bytes)|bytes إلى نص|نصوص
to_bytes|to_bytes(نص|قائمة)|إلى bytes|نصوص
to_hex|to_hex(bytes)|تمثيل ست عشري|نصوص
abs|abs(x)|القيمة المطلقة|رياضيات
floor|floor(x)|تقريب للأسفل|رياضيات
ceil|ceil(x)|تقريب للأعلى|رياضيات
round|round(x)|تقريب لأقرب عدد صحيح|رياضيات
sqrt|sqrt(x)|الجذر التربيعي|رياضيات
sign|sign(x)|الإشارة (-1, 0, 1)|رياضيات
sin|sin(x)|جيب|رياضيات
cos|cos(x)|جيب التمام|رياضيات
tan|tan(x)|ظل|رياضيات
asin|asin(x)|عكس الجيب|رياضيات
acos|acos(x)|عكس جيب التمام|رياضيات
atan|atan(x)|عكس الظل|رياضيات
atan2|atan2(y, x)|زاوية النقطة|رياضيات
log|log(x)|لوغاريتم طبيعي|رياضيات
log2|log2(x)|لوغاريتم أساس 2|رياضيات
log10|log10(x)|لوغاريتم أساس 10|رياضيات
exp|exp(x)|الأسّ e^x|رياضيات
pow|pow(x, y)|x أُس y|رياضيات
min|min(قيم…)|الأصغر|رياضيات
max|max(قيم…)|الأكبر|رياضيات
clamp|clamp(x, أدنى, أعلى)|يحصر القيمة في مجال|رياضيات
random|random([أ[, ب]])|رقم عشوائي|رياضيات
seed|seed(n)|بذرة المولّد العشوائي|رياضيات
hex|hex(n)|عدد إلى نص ست عشري|رياضيات
bin|bin(n)|عدد إلى نص ثنائي|رياضيات
error|error(قيمة)|يرمي خطأً (أي قيمة)|تحكم
assert|assert(شرط[, رسالة])|يرمي خطأً إن كان الشرط خاطئاً|تحكم
pcall|pcall(دالة, وسائط…)|يستدعي بأمان: [true, نتيجة] أو [false, خطأ]|تحكم
apply|apply(دالة, قائمة_وسائط)|يستدعي دالة بقائمة وسائط|تحكم
require|require("وحدة")|يحمّل وحدة (مرة واحدة)|تحكم
getglobal|getglobal(اسم)|قيمة متغير عام|تحكم
setglobal|setglobal(اسم, قيمة)|يضبط متغيراً عاماً|تحكم
defined|defined(اسم)|هل المتغير العام معرّف؟|تحكم
callable|callable(قيمة)|هل القيمة قابلة للاستدعاء؟|تحكم
clock|clock()|وقت المعالج بالثواني|تحكم
time|time()|الوقت الحالي (Unix)|تحكم
sleep|sleep(ثوانٍ)|ينتظر|تحكم
regex|regex(نمط[, "i"])|ينشئ كائن تعبير نمطي|أدوات اللغات
lexer|lexer(قواعد[, خيارات])|مولّد محلّل لفظي: lx.tokenize(src)|أدوات اللغات
asm|asm(اسم, عدد_الوسائط[, upvalues])|مجمّع bytecode: op/label/line/build|أدوات اللغات
opcodes|opcodes()|قائمة أوامر الآلة الافتراضية|أدوات اللغات
compile|compile(مصدر[, اسم])|يترجم مصدراً إلى دالة|أدوات اللغات
eval|eval(مصدر[, اسم])|يترجم وينفّذ|أدوات اللغات
disasm|disasm(دالة|مصدر)|تفكيك bytecode إلى نص|أدوات اللغات
serialize|serialize(دالة)|دالة إلى bytes (.exbc)|أدوات اللغات
deserialize|deserialize(bytes)|bytes إلى دالة|أدوات اللغات
save_bytecode|save_bytecode(دالة, مسار)|يحفظ bytecode في ملف|أدوات اللغات
load_bytecode|load_bytecode(مسار)|يحمّل bytecode من ملف|أدوات اللغات
`;

/* Methods of handle objects (regex / lexer / asm). */
const METHODS_SRC = `
test|r.test(نص)|هل يطابق النمط؟|regex
match|r.match(نص[, من])|أول تطابق مع المجموعات|regex
match_at|r.match_at(نص, موضع)|تطابق عند موضع محدد|regex
find_all|r.find_all(نص)|كل التطابقات|regex
replace|r.replace(نص, بديل[, عدد])|استبدال ($1, $2 مدعومة)|regex
split|r.split(نص)|تقسيم بالنمط|regex
groups|r.groups()|عدد المجموعات|regex
pattern|r.pattern()|نص النمط|regex
tokenize|lx.tokenize(نص)|قائمة رموز {type,text,line,col,pos}|lexer
op|a.op(اسم_الأمر, معاملات…)|يضيف أمراً (يُرجع a للتسلسل)|asm
label|a.label(اسم)|يعرّف وسماً للقفز|asm
line|a.line(n)|يضبط رقم السطر للأوامر التالية|asm
count|a.count()|عدد الأوامر|asm
build|a.build()|يبني دالة قابلة للاستدعاء|asm
`;

function parseRef(src, cat) {
  const out = [];
  for (const l of src.trim().split('\n')) {
    const p = l.split('|');
    if (p.length < 4) continue;
    out.push({ name: p[0], sig: p[1], doc: p[2], cat: p[3], kind: cat });
  }
  return out;
}
const REF = parseRef(REF_SRC, 'fn');
const METHODS = parseRef(METHODS_SRC, 'method');
const BUILTINS = new Set(REF.map((r) => r.name));
const REF_BY_NAME = Object.create(null);
for (const r of REF) REF_BY_NAME[r.name] = r;

const SNIPPETS = [
  { name: 'fn', sig: 'دالة', body: 'fn name(args) {\n  $0\n}' },
  { name: 'if', sig: 'شرط', body: 'if cond {\n  $0\n}' },
  { name: 'ife', sig: 'شرط/وإلا', body: 'if cond {\n  $0\n} else {\n  \n}' },
  { name: 'for', sig: 'حلقة for…in', body: 'for item in items {\n  $0\n}' },
  { name: 'forr', sig: 'حلقة على نطاق', body: 'for i in range(10) {\n  $0\n}' },
  { name: 'while', sig: 'حلقة while', body: 'while cond {\n  $0\n}' },
  { name: 'let', sig: 'متغير', body: 'let name = $0' },
  { name: 'pcall', sig: 'التقاط خطأ', body: 'let r = pcall(fn() {\n  $0\n})\nif not r[0] { print("error:", r[1]) }' },
  { name: 'closure', sig: 'عدّاد closure', body: 'fn make_counter() {\n  let n = 0\n  return fn() { n += 1  return n }\n}$0' },
  { name: 'lexer', sig: 'مولّد lexer', body: 'let lx = lexer([\n  ["_",   "\\\\s+"],\n  ["NUM", "[0-9]+"],\n  ["ID",  "[A-Za-z_\\\\x80-\\\\xff]\\\\w*"]\n])\nlet tokens = lx.tokenize(src)$0' },
  { name: 'pratt', sig: 'محلّل Pratt', body: 'let pratt = require("pratt")\nlet spec = {prefix: {}, infix: {}}\n$0' },
  { name: 'regex', sig: 'تعبير نمطي', body: 'let r = regex("$0")' },
  { name: 'asm', sig: 'مجمّع bytecode', body: 'let a = asm("name", 0)\na.op("CONSTANT", 1)\na.op("RETURN")\nlet f = a.build()$0' },
];

/* -------------------------------------------------------------- tokenizer -- */
/* Token kinds: c comment, s string, n number, w word, p punct/space. */
const TOKEN = /(\/\/[^\n]*|\/\*[\s\S]*?(?:\*\/|$))|("(?:\\[\s\S]|[^"\\\n])*"?|'(?:\\[\s\S]|[^'\\\n])*'?)|(0[xX][0-9a-fA-F]+|\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?)|([\p{L}_][\p{L}\p{N}_]*)|(\s+|[\s\S])/gu;

function tokenize(code) {
  const toks = [];
  const re = new RegExp(TOKEN.source, 'gu');
  let m;
  while ((m = re.exec(code))) {
    const s = m.index, e = re.lastIndex;
    if (m[1] !== undefined) toks.push({ t: 'c', s, e });
    else if (m[2] !== undefined) toks.push({ t: 's', s, e });
    else if (m[3] !== undefined) toks.push({ t: 'n', s, e });
    else if (m[4] !== undefined) toks.push({ t: 'w', s, e, v: m[4] });
    else toks.push({ t: 'p', s, e, v: m[5] });
    if (e === s) re.lastIndex++;
  }
  return toks;
}

const ESC = { '&': '&amp;', '<': '&lt;', '>': '&gt;' };
const esc = (s) => s.replace(/[&<>]/g, (c) => ESC[c]);

/* Returns {html, toks}. `userFns` (Set) colours functions declared in the project. */
function highlight(code, toks, userFns) {
  let out = '', prevWord = '';
  for (let i = 0; i < toks.length; i++) {
    const k = toks[i];
    const text = code.slice(k.s, k.e);
    let cls = '';
    if (k.t === 'c') cls = 'c';
    else if (k.t === 's') cls = 's';
    else if (k.t === 'n') cls = 'n';
    else if (k.t === 'w') {
      const w = k.v;
      if (KEYWORDS.has(w)) cls = 'k';
      else if (CONSTS.has(w)) cls = 't';
      else if (prevWord === 'fn') cls = 'f';
      else if (BUILTINS.has(w)) cls = 'b';
      else if (code[k.e] === '(' || (userFns && userFns.has(w))) cls = 'f';
      prevWord = w;
      out += cls ? '<span class="' + cls + '">' + esc(text) + '</span>' : esc(text);
      continue;
    }
    if (k.t === 'p' && k.v.trim()) prevWord = ''; else if (k.t !== 'p') prevWord = '';
    out += cls ? '<span class="' + cls + '">' + esc(text) + '</span>' : esc(text);
  }
  return out + '\n ';
}

/* ----------------------------------------------------- brackets & symbols -- */
const OPEN = { '(': ')', '[': ']', '{': '}' };
const CLOSE = { ')': '(', ']': '[', '}': '{' };

/* pairs: Map pos -> matching pos (both directions). unmatched: array of positions. */
function matchBrackets(toks) {
  const pairs = new Map(), stack = [], bad = [];
  for (const k of toks) {
    if (k.t !== 'p') continue;
    const ch = k.v;
    if (OPEN[ch]) stack.push({ ch, pos: k.s });
    else if (CLOSE[ch]) {
      const top = stack[stack.length - 1];
      if (top && top.ch === CLOSE[ch]) { stack.pop(); pairs.set(top.pos, k.s); pairs.set(k.s, top.pos); }
      else bad.push(k.s);
    }
  }
  for (const s of stack) bad.push(s.pos);
  return { pairs, bad };
}

function lineStarts(code) {
  const a = [0];
  for (let i = 0; i < code.length; i++) if (code.charCodeAt(i) === 10) a.push(i + 1);
  return a;
}
function lineOf(starts, pos) {
  let lo = 0, hi = starts.length - 1;
  while (lo < hi) { const mid = (lo + hi + 1) >> 1; if (starts[mid] <= pos) lo = mid; else hi = mid - 1; }
  return lo + 1;
}

/* Outline: functions (any depth) and top-level lets. */
function symbols(code, toks) {
  toks = toks || tokenize(code);
  const starts = lineStarts(code), out = [];
  const sig = toks.filter((k) => k.t !== 'c' && !(k.t === 'p' && !k.v.trim()));
  let depth = 0;
  for (let i = 0; i < sig.length; i++) {
    const k = sig[i];
    if (k.t === 'p') {
      if (k.v === '{' || k.v === '(' || k.v === '[') depth++;
      else if (k.v === '}' || k.v === ')' || k.v === ']') depth = Math.max(0, depth - 1);
      continue;
    }
    if (k.t !== 'w') continue;
    if (k.v === 'fn' && sig[i + 1] && sig[i + 1].t === 'w' && !KEYWORDS.has(sig[i + 1].v)) {
      const nm = sig[i + 1];
      let params = '', j = i + 2;
      if (sig[j] && sig[j].v === '(') {
        let e = j; while (sig[e] && sig[e].v !== ')') e++;
        params = code.slice(sig[j].e, sig[e] ? sig[e].s : code.length).replace(/\s+/g, ' ').trim();
      }
      out.push({ name: nm.v, kind: 'fn', params, line: lineOf(starts, nm.s), pos: nm.s, depth });
    } else if (k.v === 'let' && depth === 0 && sig[i + 1] && sig[i + 1].t === 'w') {
      const nm = sig[i + 1];
      out.push({ name: nm.v, kind: 'let', line: lineOf(starts, nm.s), pos: nm.s, depth });
    }
  }
  return out;
}

/* Words used in the code (for completion). */
function words(code, toks) {
  const set = new Map();
  for (const k of (toks || tokenize(code))) if (k.t === 'w' && !KEYWORDS.has(k.v)) set.set(k.v, (set.get(k.v) || 0) + 1);
  return set;
}

/* ---------------------------------------------------------- call context -- */
/* Finds the call enclosing `pos`: {name, index} or null. */
function callContext(code, toks, pos) {
  let depth = 0, commas = 0;
  let i = toks.length - 1;
  while (i >= 0 && toks[i].s >= pos) i--;
  for (; i >= 0; i--) {
    const k = toks[i];
    if (k.t !== 'p') continue;
    const ch = k.v;
    if (CLOSE[ch]) depth++;
    else if (OPEN[ch]) {
      if (depth === 0) {
        if (ch !== '(') return null;
        let j = i - 1;
        while (j >= 0 && toks[j].t === 'p' && !toks[j].v.trim()) j--;
        if (j >= 0 && toks[j].t === 'w' && !KEYWORDS.has(toks[j].v)) return { name: toks[j].v, index: commas };
        return null;
      }
      depth--;
    } else if (ch === ',' && depth === 0) commas++;
  }
  return null;
}

/* -------------------------------------------------------------- formatter -- */
/* Re-indents by bracket depth, trims trailing whitespace, keeps block comments intact. */
function format(code, indent) {
  indent = indent || '  ';
  const toks = tokenize(code);
  const starts = lineStarts(code);
  const lines = code.split('\n');
  const verbatim = new Array(lines.length).fill(false);
  // lines that begin inside a block comment (or are continuation lines of one) keep their text
  for (const k of toks) {
    if (k.t === 'c' && code[k.s + 1] === '*') {
      const a = lineOf(starts, k.s), b = lineOf(starts, Math.max(k.s, k.e - 1));
      for (let l = a + 1; l <= b; l++) verbatim[l - 1] = true;
    }
  }
  // net depth change per line, and leading closers per line
  const delta = new Array(lines.length).fill(0), lead = new Array(lines.length).fill(0), seen = new Array(lines.length).fill(false);
  for (const k of toks) {
    if (k.t === 'p' && !k.v.trim()) continue;
    const ln = lineOf(starts, k.s) - 1;
    if (k.t === 'p' && CLOSE[k.v]) {
      delta[ln]--;
      if (!seen[ln]) lead[ln]++;
    } else if (k.t === 'p' && OPEN[k.v]) { delta[ln]++; seen[ln] = true; }
    else seen[ln] = true;
    if (k.t === 'c' && code[k.s + 1] === '*') { // multi-line comment content isn't structural
      const a = lineOf(starts, k.s), b = lineOf(starts, Math.max(k.s, k.e - 1));
      for (let l = a; l <= b; l++) seen[l - 1] = true;
    }
  }
  let depth = 0;
  const out = [];
  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i];
    if (verbatim[i]) { out.push(raw.replace(/\s+$/, '')); depth = Math.max(0, depth + delta[i]); continue; }
    const t = raw.trim();
    if (!t) { out.push(''); depth = Math.max(0, depth + delta[i]); continue; }
    const d = Math.max(0, depth - lead[i]);
    out.push(indent.repeat(d) + t);
    depth = Math.max(0, depth + delta[i]);
  }
  // collapse >2 blank lines, single trailing newline
  let res = out.join('\n').replace(/\n{4,}/g, '\n\n\n').replace(/\s+$/, '');
  return res + '\n';
}

/* ------------------------------------------------------------ diagnostics -- */
/* Parse "[file:LINE] message" lines of compiler/runtime output. */
function parseDiagnostics(text) {
  const out = [], re = /^[ \t]*\[([^\]:\n]+):(\d+)\][ \t]*(.*)$/;
  let last = '';
  for (const line of text.split('\n')) {
    const m = re.exec(line);
    if (!m) { if (line.trim()) last = line.trim(); continue; }
    // runtime stack frames ("in <fn>") borrow the error message printed above them
    out.push({ file: m[1], line: +m[2], msg: /^in\s/.test(m[3]) && last ? last + ' (' + m[3] + ')' : m[3] });
  }
  return out;
}

root.EXB_LANG = { KEYWORDS, CONSTS, BUILTINS, REF, METHODS, REF_BY_NAME, SNIPPETS, tokenize, highlight, matchBrackets,
  symbols, words, callContext, format, parseDiagnostics, lineStarts, lineOf, esc, OPEN, CLOSE };
})(typeof self !== 'undefined' ? self : this);
