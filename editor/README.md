# محرر EXBIN / EXBIN Editor

محرر أكواد للغة EXBIN يعمل **بدون إنترنت** على كل المنصات: Windows وmacOS وLinux وAndroid وiOS.
محرك EXBIN (نفس شيفرة C) مُترجَم إلى **WebAssembly** ويعمل داخل المتصفح في Web Worker،
فلا حاجة لتثبيت أي شيء ولا لخادم.

## الميزات
- تلوين الصيغة (كلمات مفتاحية، دوال مدمجة، نصوص، تعليقات، أرقام) مع دعم العربية.
- إكمال الأقواس والاقتباس، إزاحة تلقائية، `Tab`، تعليق/إلغاء تعليق `Ctrl+/`.
- شريط رموز سريع للهاتف: `{ } ( ) [ ] " = ; …` مع تراجع/إعادة.
- تشغيل بـ `Ctrl+Enter` أو زر «تشغيل»، وزر «إيقاف» يقطع الحلقات اللانهائية.
- عرض الـ bytecode (disassembly).
- أخطاء بسطر قابل للنقر: يُسطَّر السطر ويُقفز إليه.
- عدة ملفات في تبويبات؛ `require("اسم")` تجد الملفات المفتوحة، والمكتبتان `pratt` و`pretty` جاهزتان.
  `read_file` / `write_file` تعمل على نظام ملفات مؤقت في الذاكرة (يُمسح عند كل تشغيل).
- أمثلة جاهزة، وضع فاتح/داكن، حجم خط، حفظ تلقائي على الجهاز، فتح/تنزيل ملفات `.exb`.
- PWA: قابل للتثبيت كتطبيق ويعمل offline.

## التشغيل
المتصفحات تمنع WebAssembly/Workers من `file://`، لذا قدِّمه عبر خادم (أي خادم ثابت):

```sh
cd editor && ./serve.sh 8080      # ثم افتح http://localhost:8080
```

انشره كما هو على أي استضافة ثابتة — يجب أن يكون HTTPS ليعمل «التثبيت».

**GitHub Pages:** الملف `.github/workflows/pages.yml` ينشر المجلد تلقائياً عند كل push إلى `main`. فعّله مرة واحدة من Settings ← Pages ← Source: **GitHub Actions**، فيصير الرابط `https://USER.github.io/REPO/`.

## على Android
1. **الأسهل:** افتح الرابط في Chrome ← القائمة ← «تثبيت التطبيق / إضافة إلى الشاشة الرئيسية».
   يعمل بعدها كتطبيق مستقل وبدون إنترنت.
2. **ملف APK (اختياري):** مجلد `android/` فيه إعداد [Capacitor](https://capacitorjs.com):
   ```sh
   cd editor/android
   npm install
   npx cap add android      # مرة واحدة
   npx cap sync
   npx cap open android     # يفتح Android Studio ← Build ← Build APK
   ```
   (يحتاج Node وAndroid Studio. لم يُبنَ APK داخل هذه البيئة.)

## إعادة بناء الملفات المولَّدة
- `exbin.wasm`: `WASI_SYSROOT=/path/to/wasi-sysroot ./wasm/build.sh`
- `data.js` (الأمثلة والمكتبات): `node tools/build-data.mjs`
- عند تعديل أي ملف ارفع `VERSION` في `sw.js` ليتحدّث الكاش.

## البنية
```
index.html  style.css  app.js     الواجهة والمحرر
worker.js                         يشغّل wasm + نظام ملفات افتراضي
exbin.wasm                        محرك EXBIN (~260KB)
sw.js manifest.webmanifest icons/ PWA
wasm/                             glue.c و build.sh لبناء exbin.wasm
android/                          إعداد Capacitor
```
