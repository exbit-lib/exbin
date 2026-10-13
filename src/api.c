/* EXBIN: implementation of the public C API declared in exbin.h. */
#include <stdarg.h>
#include <time.h>

#include "vm.h"

/* ------------------------------------------------------------- lifecycle -- */
ExVM* ex_new(void) {
  VM* vm = (VM*)calloc(1, sizeof(VM));
  if (!vm) return NULL;
  vm->stackTop = vm->stack;
  vm->errval = NIL_VAL;
  vm->nextGC = 1u << 20;
  vm->rng = ((uint64_t)time(NULL) << 20) ^ (uint64_t)(uintptr_t)vm ^ 0x9E3779B97F4A7C15ULL;
  vm->globals = map_new(vm);
  vm->modules = map_new(vm);
  natives_register(vm);
  toolkit_register(vm);
  size_t next = vm->bytesAllocated * 2;
  vm->nextGC = next < (1u << 20) ? (1u << 20) : next;
  return vm;
}

void ex_free(ExVM* vm) {
  if (!vm) return;
  gc_free_all(vm);
  for (int i = 0; i < vm->libCount; i++) free(vm->libPaths[i]);
  free(vm->libPaths);
  buf_free(&vm->trace);
  free(vm->errmsg);
  free(vm);
}

void ex_set_output(ExVM* vm, ExWriteFn fn, void* ud) {
  vm->out = fn;
  vm->outUd = ud;
}

void ex_set_args(ExVM* vm, int argc, const char* const* argv) {
  ObjList* l = list_new(vm);
  for (int i = 0; i < argc; i++) list_push(vm, l, OBJ_VAL(str_from(vm, argv[i])));
  vm_define_global(vm, "args", OBJ_VAL(l));
}

void ex_add_lib_path(ExVM* vm, const char* dir) {
  vm->libPaths = (char**)realloc(vm->libPaths, sizeof(char*) * (size_t)(vm->libCount + 1));
  size_t n = strlen(dir);
  char* c = (char*)malloc(n + 1);
  memcpy(c, dir, n + 1);
  vm->libPaths[vm->libCount++] = c;
}

void ex_set_gc_stress(ExVM* vm, int on) { vm->gcStress = on != 0; }

/* --------------------------------------------------------------- running -- */
static int run_compiled(VM* vm, ObjFunction* fn, Value* result) {
  if (!fn) return EX_COMPILE_ERROR;
  return vm_exec_function(vm, fn, result);
}

int ex_eval(ExVM* vm, const char* source, const char* name, ExValue* result) {
  vm->cdepth = 0;
  return run_compiled(vm, compile_source(vm, source, name ? name : "<eval>"), result);
}

int ex_run_string(ExVM* vm, const char* source, const char* name) {
  return ex_eval(vm, source, name ? name : "<string>", NULL);
}

int ex_run_file(ExVM* vm, const char* path) {
  size_t n;
  char* src = vm_read_file(path, &n);
  if (!src) {
    char msg[512];
    snprintf(msg, sizeof msg, "Could not open file '%s'.", path);
    vm_set_errmsg(vm, msg);
    return EX_COMPILE_ERROR;
  }
  vm->cdepth = 0;
  ObjFunction* fn;
  if (bytecode_check_magic(src, n)) fn = bytecode_read(vm, (const uint8_t*)src, n);
  else fn = compile_source(vm, src, path);
  free(src);
  return run_compiled(vm, fn, NULL);
}

const char* ex_error_message(ExVM* vm) { return vm->errmsg ? vm->errmsg : ""; }

/* -------------------------------------------------------- bytecode tools -- */
int ex_compile_to_file(ExVM* vm, const char* source, const char* name, const char* out_path) {
  ObjFunction* fn = compile_source(vm, source, name ? name : "<source>");
  if (!fn) return EX_COMPILE_ERROR;
  Buf b;
  buf_init(&b);
  if (!bytecode_write(vm, fn, &b)) {
    Buf m;
    buf_init(&m);
    value_tostr_buf(vm, &m, vm->errval);
    vm_set_errmsg(vm, buf_str(&m));
    buf_free(&m);
    buf_free(&b);
    return EX_COMPILE_ERROR;
  }
  FILE* f = fopen(out_path, "wb");
  if (!f) {
    char msg[512];
    snprintf(msg, sizeof msg, "Could not write '%s'.", out_path);
    vm_set_errmsg(vm, msg);
    buf_free(&b);
    return EX_COMPILE_ERROR;
  }
  fwrite(b.d, 1, b.len, f);
  fclose(f);
  buf_free(&b);
  return EX_OK;
}

char* ex_disassemble(ExVM* vm, const char* source, const char* name) {
  ObjFunction* fn = compile_source(vm, source, name ? name : "<source>");
  if (!fn) return NULL;
  Buf b;
  buf_init(&b);
  chunk_disassemble(fn, &b);
  return buf_take(&b);
}

char* ex_disassemble_file(ExVM* vm, const char* path) {
  size_t n;
  char* src = vm_read_file(path, &n);
  if (!src) {
    char msg[512];
    snprintf(msg, sizeof msg, "Could not open file '%s'.", path);
    vm_set_errmsg(vm, msg);
    return NULL;
  }
  ObjFunction* fn = bytecode_check_magic(src, n) ? bytecode_read(vm, (const uint8_t*)src, n)
                                                 : compile_source(vm, src, path);
  free(src);
  if (!fn) return NULL;
  Buf b;
  buf_init(&b);
  chunk_disassemble(fn, &b);
  return buf_take(&b);
}

void ex_free_string(char* s) { free(s); }

/* ---------------------------------------------------- natives & globals -- */
int ex_def_native(ExVM* vm, const char* name, ExNative fn, void* ud, int min_args, int max_args,
                  void (*ud_free)(void*)) {
  vm_define_global(vm, name, OBJ_VAL(native_new(vm, name, fn, ud, min_args, max_args, ud_free)));
  return 1;
}

int ex_error(ExVM* vm, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vm_verror(vm, fmt, ap);
  va_end(ap);
  return 0;
}

int ex_get_global(ExVM* vm, const char* name, ExValue* out) {
  Value v;
  if (!map_get(vm->globals, OBJ_VAL(str_from(vm, name)), &v)) return 0;
  if (out) *out = v;
  return 1;
}

void ex_set_global(ExVM* vm, const char* name, ExValue v) { vm_define_global(vm, name, v); }

int ex_call(ExVM* vm, ExValue fn, int argc, const ExValue* args, ExValue* out) {
  Value r = NIL_VAL;
  if (!vm_call(vm, fn, argc, args, &r)) {
    Buf b;
    buf_init(&b);
    buf_adds(&b, "Runtime error: ");
    value_tostr_buf(vm, &b, vm->errval);
    buf_addc(&b, '\n');
    if (vm->trace.d) buf_add(&b, vm->trace.d, vm->trace.len);
    vm_set_errmsg(vm, buf_str(&b));
    buf_free(&b);
    return 0;
  }
  if (out) *out = r;
  return 1;
}

/* --------------------------------------------------------- construction --- */
ExValue ex_nil(void) { return NIL_VAL; }
ExValue ex_bool(int b) { return BOOL_VAL(b); }
ExValue ex_num(double n) { return NUM_VAL(n); }
ExValue ex_str(ExVM* vm, const char* s, size_t len) { return OBJ_VAL(str_copy(vm, s, len)); }
ExValue ex_cstr(ExVM* vm, const char* s) { return OBJ_VAL(str_from(vm, s)); }
ExValue ex_list_new(ExVM* vm) { return OBJ_VAL(list_new(vm)); }
void ex_list_push(ExVM* vm, ExValue list, ExValue v) {
  if (IS_LIST(list)) list_push(vm, AS_LIST(list), v);
}
ExValue ex_map_new(ExVM* vm) { return OBJ_VAL(map_new(vm)); }
void ex_map_set(ExVM* vm, ExValue map, const char* key, ExValue v) {
  if (IS_MAP(map)) map_set_str(vm, AS_MAP(map), key, v);
}
ExValue ex_bytes_new(ExVM* vm, const void* data, size_t len) {
  ObjBytes* b = bytes_new(vm, (int)len);
  if (len && data) memcpy(b->data, data, len);
  return OBJ_VAL(b);
}

/* ----------------------------------------------------------- inspection --- */
int ex_is_nil(ExValue v) { return IS_NIL(v); }
int ex_is_bool(ExValue v) { return IS_BOOL(v); }
int ex_is_num(ExValue v) { return IS_NUM(v); }
int ex_is_string(ExValue v) { return IS_STRING(v); }
int ex_is_list(ExValue v) { return IS_LIST(v); }
int ex_is_map(ExValue v) { return IS_MAP(v); }
int ex_is_bytes(ExValue v) { return IS_BYTES(v); }
int ex_is_callable(ExValue v) { return IS_CALLABLE(v); }
int ex_truthy(ExValue v) { return value_truthy(v); }
int ex_as_bool(ExValue v) { return IS_BOOL(v) ? AS_BOOL(v) : 0; }
double ex_as_num(ExValue v) { return IS_NUM(v) ? AS_NUM(v) : 0; }
const char* ex_as_string(ExValue v, size_t* len) {
  if (!IS_STRING(v)) {
    if (len) *len = 0;
    return NULL;
  }
  if (len) *len = (size_t)AS_STRING(v)->len;
  return AS_CSTRING(v);
}
const uint8_t* ex_as_bytes(ExValue v, size_t* len) {
  if (!IS_BYTES(v)) {
    if (len) *len = 0;
    return NULL;
  }
  if (len) *len = (size_t)AS_BYTES(v)->len;
  return AS_BYTES(v)->data;
}
int ex_list_len(ExValue list) { return IS_LIST(list) ? AS_LIST(list)->count : 0; }
ExValue ex_list_get(ExValue list, int i) {
  if (!IS_LIST(list) || i < 0 || i >= AS_LIST(list)->count) return NIL_VAL;
  return AS_LIST(list)->items[i];
}
int ex_map_len(ExValue map) { return IS_MAP(map) ? AS_MAP(map)->count : 0; }
ExValue ex_map_keys(ExVM* vm, ExValue map) {
  ObjList* l = list_new(vm);
  if (IS_MAP(map)) {
    ObjMap* m = AS_MAP(map);
    for (int i = 0; i < m->used; i++)
      if (!IS_NIL(m->entries[i].key)) list_push(vm, l, m->entries[i].key);
  }
  return OBJ_VAL(l);
}
int ex_map_get(ExValue map, const char* key, ExValue* out) {
  if (!IS_MAP(map)) return 0;
  ObjMap* m = AS_MAP(map);
  size_t kl = strlen(key);
  for (int i = 0; i < m->used; i++) {
    Value k = m->entries[i].key;
    if (IS_STRING(k) && (size_t)AS_STRING(k)->len == kl && memcmp(AS_CSTRING(k), key, kl) == 0) {
      if (out) *out = m->entries[i].value;
      return 1;
    }
  }
  return 0;
}
const char* ex_type_name(ExValue v) { return value_type_name(v); }
ExValue ex_to_string(ExVM* vm, ExValue v) { return OBJ_VAL(value_tostring(vm, v)); }
ExValue ex_repr(ExVM* vm, ExValue v) { return OBJ_VAL(value_repr(vm, v)); }
