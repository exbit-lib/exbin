/* EXBIN runtime worker: runs the WebAssembly VM off the UI thread so that
   "Stop" can simply terminate it, and gives it a tiny in-memory file system
   (so require(), read_file() and write_file() work). */
if (typeof importScripts === 'function' && !self.EXB_DATA) importScripts('data.js');

const dec = new TextDecoder(), enc = new TextEncoder();
let modPromise = null;
function getModule() {
  if (!modPromise) {
    // Standalone mode (opened from file://): the wasm is embedded as base64 by tools/build-inline.mjs.
    modPromise = (self.EXB_WASM_B64
      ? Promise.resolve(Uint8Array.from(atob(self.EXB_WASM_B64), (c) => c.charCodeAt(0)))
      : fetch('exbin.wasm').then((r) => r.arrayBuffer())).then((b) => WebAssembly.compile(b));
  }
  return modPromise;
}

function normPath(p) {
  const out = [];
  for (const s of p.split('/')) {
    if (!s || s === '.') continue;
    if (s === '..') out.pop(); else out.push(s);
  }
  return '/' + out.join('/');
}

function makeRuntime(userFiles, stdinText) {
  const stdin = enc.encode(stdinText || ''); let stdinPos = 0;
  const fs = new Map(); // path -> {d: Uint8Array, n: length}
  const put = (p, text) => { const d = enc.encode(text); fs.set(normPath(p), { d, n: d.length }); };
  for (const [k, v] of Object.entries(self.EXB_DATA.lib)) put('/lib/' + k, v);
  for (const [k, v] of Object.entries(userFiles || {})) put('/' + k, v);

  const open = new Map(); // fd -> {f, pos, w, append}
  let nextFd = 10;
  let mem = null;
  const view = () => new DataView(mem.buffer);
  const u8 = () => new Uint8Array(mem.buffer);

  const E_BADF = 8, E_NOENT = 44, E_SPIPE = 70, E_INVAL = 28;
  const wasi = {
    environ_sizes_get: (c, s) => { view().setUint32(c, 0, true); view().setUint32(s, 0, true); return 0; },
    environ_get: () => 0,
    clock_time_get: (id, prec, out) => {
      const ms = id === 0 ? Date.now() : performance.now();
      view().setBigUint64(out, BigInt(Math.round(ms * 1e6)), true);
      return 0;
    },
    fd_close: (fd) => { open.delete(fd); return 0; },
    fd_fdstat_get: (fd, p) => {
      const v = view();
      v.setUint8(p, fd < 3 ? 2 : (fd === 3 ? 3 : 4));
      v.setUint16(p + 2, 0, true);
      v.setBigUint64(p + 8, 0xffffffffffffffffn, true);
      v.setBigUint64(p + 16, 0xffffffffffffffffn, true);
      return 0;
    },
    fd_fdstat_set_flags: () => 0,
    fd_prestat_get: (fd, p) => {
      if (fd !== 3) return E_BADF;
      view().setUint8(p, 0); view().setUint32(p + 4, 1, true); return 0;
    },
    fd_prestat_dir_name: (fd, p, len) => { if (fd !== 3) return E_BADF; u8()[p] = 47; return 0; },
    fd_read: (fd, iovs, n, outp) => {
      const v = view(); let total = 0;
      const o = open.get(fd);
      if (o) {
        for (let i = 0; i < n; i++) {
          const buf = v.getUint32(iovs + i * 8, true), len = v.getUint32(iovs + i * 8 + 4, true);
          const c = Math.max(0, Math.min(len, o.f.n - o.pos));
          u8().set(o.f.d.subarray(o.pos, o.pos + c), buf);
          o.pos += c; total += c;
          if (c < len) break;
        }
      } else if (fd === 0) {
        for (let i = 0; i < n; i++) {
          const buf = v.getUint32(iovs + i * 8, true), len = v.getUint32(iovs + i * 8 + 4, true);
          const c = Math.max(0, Math.min(len, stdin.length - stdinPos));
          u8().set(stdin.subarray(stdinPos, stdinPos + c), buf);
          stdinPos += c; total += c;
          if (c < len) break;
        }
      } else return E_BADF;
      v.setUint32(outp, total, true); return 0;
    },
    fd_seek: (fd, off, whence, outp) => {
      const o = open.get(fd); if (!o) return E_SPIPE;
      off = Number(off);
      const np = whence === 0 ? off : whence === 1 ? o.pos + off : o.f.n + off;
      if (np < 0) return E_INVAL;
      o.pos = np; view().setBigUint64(outp, BigInt(np), true); return 0;
    },
    fd_write: (fd, iovs, n, outp) => {
      const v = view(); let total = 0;
      const o = open.get(fd);
      for (let i = 0; i < n; i++) {
        const buf = v.getUint32(iovs + i * 8, true), len = v.getUint32(iovs + i * 8 + 4, true);
        if (o) {
          if (!o.w) return E_BADF;
          if (o.append) o.pos = o.f.n;
          const need = o.pos + len;
          if (need > o.f.d.length) { const nd = new Uint8Array(Math.max(need, o.f.d.length * 2, 64)); nd.set(o.f.d.subarray(0, o.f.n)); o.f.d = nd; }
          o.f.d.set(u8().subarray(buf, buf + len), o.pos);
          o.pos += len; if (o.pos > o.f.n) o.f.n = o.pos;
        } else if (fd === 1 || fd === 2) {
          extra.push(dec.decode(u8().subarray(buf, buf + len)));
        } else return E_BADF;
        total += len;
      }
      v.setUint32(outp, total, true); return 0;
    },
    path_open: (dirfd, dflags, pp, pl, oflags, rb, ri, fdflags, outp) => {
      if (dirfd !== 3) return E_BADF;
      const p = normPath(dec.decode(u8().subarray(pp, pp + pl)));
      let f = fs.get(p);
      const write = (BigInt(rb) & 64n) !== 0n || (oflags & 9) !== 0;
      if (!f) {
        if (!(oflags & 1)) return E_NOENT;
        f = { d: new Uint8Array(64), n: 0 }; fs.set(p, f);
      } else if (oflags & 8) f.n = 0;
      const fd = nextFd++;
      open.set(fd, { f, pos: 0, w: write, append: (fdflags & 1) !== 0 });
      view().setUint32(outp, fd, true); return 0;
    },
    proc_exit: (code) => { const e = new Error('exit'); e.exitCode = code; throw e; },
  };
  const extra = [];
  return { wasi, extra, setMem: (m) => { mem = m; } };
}

async function run(msg) {
  const t0 = performance.now();
  const mod = await getModule();
  const rt = makeRuntime(msg.files, msg.stdin);
  const inst = await WebAssembly.instantiate(mod, { wasi_snapshot_preview1: rt.wasi });
  const x = inst.exports;
  rt.setMem(x.memory);
  const bytes = enc.encode(msg.src);
  const ptr = x.exb_alloc(bytes.length + 1);
  new Uint8Array(x.memory.buffer).set(bytes, ptr);
  new Uint8Array(x.memory.buffer)[ptr + bytes.length] = 0;
  let status = 0, exit = null;
  try {
    status = (msg.cmd === 'disasm' || msg.cmd === 'check') ? x.exb_disasm(ptr) : x.exb_run(ptr);
  } catch (e) {
    if (e && e.exitCode !== undefined) exit = e.exitCode; else throw e;
  }
  const out = dec.decode(new Uint8Array(x.memory.buffer, x.exb_out(), x.exb_len())) + rt.extra.join('');
  return { status, out, exit, ms: performance.now() - t0, version: dec.decode(new Uint8Array(x.memory.buffer, x.exb_version(), 16)).split('\0')[0] };
}

self.onmessage = async (e) => {
  const m = e.data;
  try {
    if (m.cmd === 'init') {
      await getModule();
      self.postMessage({ id: m.id, ready: true });
      return;
    }
    const r = await run(m);
    self.postMessage({ id: m.id, cmd: m.cmd, ...r });
  } catch (err) {
    self.postMessage({ id: m.id, status: 3, out: 'Internal error: ' + (err && err.message || err), ms: 0 });
  }
};
