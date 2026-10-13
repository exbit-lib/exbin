<p align="center"><img src="assets/exbin-icon.svg" width="160" alt="EXBIN logo"></p>

# EXBIN

**EXBIN** لغة برمجة مخصّصة لـ **بناء لغات البرمجة والأدوات**. النواة آلة افتراضية (VM) حقيقية بلغة **C**، مع واجهة **C++17**، ورؤوس `.h` / `.hpp` للتضمين في أي مشروع (بما فيه Android عبر NDK).

An embeddable scripting language whose job is to help you build *other* languages and tools: regex engine, lexer generator, Pratt-parser kit, bytecode assembler, verifier, serializer — all on a real stack VM.

```
 مصدر EXBIN (.exb) ──► scanner ► Pratt compiler ──► bytecode ──► verifier ──► VM
                                                         ▲            │
 لغتك الجديدة (مكتوبة بـ EXBIN):                         │            ▼
   lexer(...)  ►  pratt parser  ►  AST  ►  asm(...)  ────┘      .exbc (ملف bytecode)
```

## محرر الأكواد / Editor

محرر يعمل على كل المنصات (بما فيها Android) وبدون إنترنت: انظر [`editor/`](editor/README.md) — `cd editor && ./serve.sh`.

## البناء / Build

```sh
make            # build/exbin + build/libexbin.a   (يحتاج gcc/clang + make فقط)
make test       # كل الاختبارات + أمثلة التضمين C/C++
make test-asan  # نفس الاختبارات تحت ASan/UBSan مع GC في وضع الإجهاد
sudo make install
```

لا توجد أي اعتماديات خارجية. الملفات: `include/exbin.h` (واجهة C) و`include/exbin.hpp` (واجهة C++) و`src/*.c` (النواة).

## الاستخدام / CLI

```sh
exbin                       # REPL
exbin file.exb [args...]    # تشغيل مصدر أو bytecode
exbin -e 'print(1 + 2)'     # تشغيل سطر
exbin -c file.exb -o f.exbc # ترجمة إلى bytecode
exbin -d f.exbc             # تفكيك (disassembly) لمصدر أو bytecode
exbin -L dir file.exb       # مسار بحث إضافي لـ require  (أو EXBIN_PATH)
```

## جولة سريعة في اللغة

```
let name = "EXBIN"                      // متغيّر
fn fact(n) { if n <= 1 { return 1 } return n * fact(n - 1) }

let xs = [3, 1, 2]                      // قائمة
let m  = {a: 1, "b c": 2, [1 + 1]: 3}   // خريطة (تحافظ على ترتيب الإدخال)

for x in xs.sort() { print(x) }         // for … in  (قوائم، خرائط، نطاقات، نصوص، bytes)
for i in range(10) { if i % 2 == 0 { continue }  print(i) }
while cond { … break … }

fn make_counter() {                     // closures حقيقية
  let n = 0
  return fn() { n += 1  return n }
}
```

* **الأنواع**: `nil`, `bool`, `number` (double), `string` (بايتات UTF-8), `list`, `map`, `bytes`, `range`, `function`.
* **المعاملات**: `+ - * / % **` (الأسّ يمين-ترابطي وأقوى من النفي: `-2 ** 2 == -4`) و`& | ^ ~ << >>` و`== != < <= > >=` و`and or not` (`&&` `||` `!` أيضًا) والإسناد المركّب `+= -= *= /= %=` على المتغيّرات والحقول والعناصر.
* **المساواة** `==` بنيوية للقوائم والخرائط. الكل "حقيقي" truthy ما عدا `nil` و`false` (الصفر حقيقي).
* **النصوص**: `"a" + 1` تعطي `"a1"`، `"ab" * 3`، فهرسة بالبايت `s[i]`، شرائح `s[range(1, 3)]`، و`for c in s` تمرّ على **حروف UTF-8** (العربية تعمل مباشرة، والمعرّفات يمكن أن تكون عربية).
* عدد الوسائط عند الاستدعاء **مطابق تمامًا** للتعريف. لا يوجد عامل `?:` ولا `try` — استخدم `pcall`.
* السطر الذي يبدأ بـ `(` أو `[` أو `-` يبدأ جملة جديدة (لا يكمل السطر السابق)، والفاصلة المنقوطة اختيارية.
* `m.len` / `xs.len` / `s.len` خاصية الطول. الدوال متاحة كدوال وكطرق: `len(x)` = `x.len()`، `upper(s)` = `s.upper()`.

## مكتبة المعيار

| المجال | الدوال |
|---|---|
| إدخال/إخراج | `print write input read_file read_bytes write_file append_file file_exists env exit args` |
| أنواع/تحويل | `type str repr num int list bytes format` |
| حاويات | `len push pop insert remove delete keys values items has find copy slice join reverse sort map filter reduce each any all sum zip enumerate range` |
| نصوص | `split lines chars replace upper lower trim trim_start trim_end starts_with ends_with repeat pad_left pad_right substr chr ord utf8_len` |
| رياضيات | `abs floor ceil round sqrt sign sin cos tan asin acos atan atan2 log log2 log10 exp pow min max clamp random seed hex bin PI E INF` |
| تحكم | `error assert pcall apply require getglobal setglobal defined callable clock time sleep` |

`format("%-8s|%5.2f|%d|%x|%r", …)` — عرض/دقة `%s` تُحسب بـ**حروف** UTF-8 لا بايتات، فالمحاذاة صحيحة مع العربية.

الأخطاء: `error(v)` ترمي أي قيمة؛ `pcall(f, args…)` ترجع `[true, نتيجة]` أو `[false, خطأ]`. الأخطاء غير الملتقطة تطبع رسالة مع **stack trace**.

الوحدات: `require("path")` يبحث نسبةً إلى الملف الطالب ثم `-L`/`EXBIN_PATH`؛ تُحمَّل مرة واحدة وتُخزَّن، ويُكتشف الاعتماد الدائري. الوحدة تُرجع قيمتها بـ `return {...}`.

## أدوات بناء اللغات (قلب EXBIN)

### 1) regex
```
let r = regex("(\\d+)-(\\d+)", "i")       // flags: i
r.test(s)  r.match(s [, from])  r.match_at(s, pos)  r.find_all(s)
r.replace(s, "$2-$1" [, count])  r.split(s)  r.groups()  r.pattern()
```
محرك backtracking ذاتي: مجموعات `(..)` و`(?:..)`، تناوب `|`، `* + ? {m,n}` (وكسولة `*?`)، فئات `[a-z]` `[^..]`، `\d \w \s \b`، `\xHH`، `^ $`. **`\w` و`[\x80-\xff]` يغطّيان بايتات UTF-8** فتعمل المعرّفات العربية. له حدود خطوات/ذاكرة تمنع التعليق مع الأنماط الخبيثة.

### 2) lexer generator
```
let lx = lexer([
  ["_",     "\\s+"],                       // الاسم "_" (أو nil) = تجاهل
  ["NUM",   "[0-9]+(\\.[0-9]+)?"],
  ["IDENT", "[A-Za-z_\\x80-\\xff]\\w*"],
  ["+",     "\\+"], ["==", "=="], ["=", "="]
], {keywords: {let: "LET"}, recover: false})
lx.tokenize(src)   // → [{type, text, line, col, pos}, …, {type:"EOF"}]
```
أطول مطابقة تفوز (maximal munch) وعند التعادل أول قاعدة. `line`/`col` يُحسبان بالحروف.

### 3) Pratt parser (`lib/pratt.exb`، مكتوب بـ EXBIN نفسها)
```
let pratt = require("pratt")
spec.prefix["NUM"] = fn(p, t) { return {type: "num", value: num(t.text)} }
spec.infix["+"]    = pratt.binary(10, false)       // أولوية 10، ترابط يساري
spec.infix["**"]   = pratt.binary(40, true)        // يميني
let ast = pratt.parser(lx.tokenize(src), spec).parse(0)
```
الكائن `p`: `peek next check match expect(type, msg) parse(prec) at_end fail(msg)`، وأدوات `pratt.unary / postfix / separated`. و`lib/pretty.exb` لطباعة الـ AST.

### 4) Bytecode assembler → كود قابل للتشغيل
```
let a = asm("fact", 1)                    // الاسم، عدد الوسائط [، عدد upvalues]
a.op("CONSTANT", 1)
a.label("top")
a.op("GET_LOCAL", 1).op("CONSTANT", 1).op("GREATER").op("JUMP_IF_FALSE", "done").op("POP")
…
a.op("LOOP", "top")
let f = a.build()                          // دالة عادية: f(10)
```
`a.op(NAME, operands…)` `a.label(n)` `a.line(n)` `a.count()` `a.build()` — و`opcodes()` تعيد قائمة الأوامر. الـ **verifier** يرفض أي برنامج فيه أمر غير صالح أو قفزة خاطئة أو **stack underflow أو عمق مكدّس غير متّسق** — لذلك البايتكود المُتحقَّق منه لا يستطيع إفساد الـ VM.

### 5) أدوات الـ bytecode
`compile(src)` · `eval(src)` · `disasm(fn|src)` · `serialize(fn)`/`deserialize(bytes)` · `save_bytecode(fn, path)`/`load_bytecode(path)` · `fn_info(fn)`.
تنسيق `.exbc`: ترويسة `EXBC` + نسخة، ثم شجرة الدوال (varint + جدول أسطر مضغوط RLE + ثوابت). القارئ **صلب**: حدود لكل حقل، وفحص كامل بعد القراءة (يُختبر بـ fuzzing في `tests/serialize.exb`).

## الأمثلة (`examples/`)

| الملف | ما يوضّحه |
|---|---|
| `hello.exb` | جولة سريعة |
| `calc.exb` | **لغة كاملة** (lexer + Pratt + مفسّر) في ~80 سطرًا، متغيّرات ودوال ومعرّفات عربية |
| `tinylang.exb` | لغة تُترجم إلى **bytecode حقيقي** عبر `asm`، تُشغَّل وتُحفظ كـ `.exbc` |
| `json.exb` | محلّل JSON + طابعة جميلة مبنيان على مولّد الـ lexer |
| `embed.c` / `embed.cpp` | تضمين EXBIN في برنامج C / C++ |

```sh
build/exbin examples/calc.exb
build/exbin examples/calc.exb "1+2" "x = 3" "x ** 2"
build/exbin examples/tinylang.exb
```

## التضمين / Embedding

### C — `exbin.h`
```c
ExVM* vm = ex_new();
ex_def_native(vm, "hypot", my_native, NULL, 2, 2, NULL);
if (ex_run_string(vm, "print(hypot(3, 4))", "main") != EX_OK)
  fprintf(stderr, "%s", ex_error_message(vm));
ExValue f; ex_get_global(vm, "area", &f);
ExValue out; ex_call(vm, f, 2, args, &out);
ex_free(vm);
```
النواتف (natives): `int fn(ExVM*, int argc, const ExValue* args, ExValue* ret, void* ud)` — أرجع 1 للنجاح، أو استدعِ `ex_error(vm, "…")` وأرجع 0.

### C++ — `exbin.hpp`
```cpp
exbin::VM vm;
vm.setOutput([&](const std::string& s) { out += s; });       // إعادة توجيه print
vm.defNative("shout", 1, 1, [](exbin::VM& vm, auto& a, exbin::Value& ret) {
  ret = exbin::Value::string(vm, a[0].asString() + "!");  return true; });
vm.run("print(shout('hi'))");
```

**قاعدة الـ GC المهمة للمُضمِّنين:** يعمل الـ GC فقط داخل حلقة التنفيذ (عند الاستدعاءات والقفزات الخلفية)، لذا القيم التي تنشئها بالواجهة صالحة حتى تُشغّل الـ VM مرة أخرى. خزّنها بـ `ex_set_global` إن أردت إبقاءها. ونواتفك التي تستدعي `ex_call`/`vm_call` يجب ألا تحتفظ بكائنات غير مربوطة بالـ VM عبر الاستدعاء.

## تصميم الـ VM

* آلة مكدّس (stack machine)، 51 أمرًا (انظر `src/chunk.h` أو `opcodes()`)، معاملات 16-bit big-endian، **closures** بـ upvalues مفتوحة/مغلقة، التقاط لكل تكرار حلقة.
* **GC** mark & sweep مع عتبة تكيّفية، يعمل فقط عند نقاط آمنة (safepoints)، ووضع إجهاد `EXBIN_GC_STRESS=1` يجمع عند كل نقطة.
* **المُتحقّق** (`chunk_verify`): تحليل تدفّق لعمق المكدّس عبر كل مسارات التحكم؛ يحسب `maxStack` لكل دالة فيُفحص الفائض عند الاستدعاء بدل كل أمر.
* خرائط مرتّبة الإدخال (index + entries)، قيم `double` موسومة، نصوص بتجزئة مخزّنة.
* حدود: عمق استدعاء 1024 إطارًا، مكدّس 262144 قيمة، تداخل natives→VM حتى 160.
* الأداء المرجعي على جهاز الاختبار: `fib(32)` ≈ 0.3 ث، حلقة 5 ملايين تكرار ≈ 0.3 ث.

## بنية المشروع

```
include/exbin.h, exbin.hpp     الواجهات العامة
src/scanner.c compiler.c       مصدر → bytecode (Pratt، مرور واحد)
src/chunk.c                    الأوامر، المُتحقّق، المفكِّك
src/vm.c                       حلقة التنفيذ، الاستدعاءات، upvalues
src/object.c                   الكائنات، الخرائط، الطباعة، GC
src/serialize.c                تنسيق .exbc
src/regex.c                    محرك regex
src/natives.c toolkit.c        المكتبة القياسية وأدوات بناء اللغات
src/api.c main.cpp             واجهة C وCLI/REPL
lib/pratt.exb pretty.exb       مكتبات مكتوبة بـ EXBIN
tests/ examples/
```
