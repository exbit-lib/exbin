/*
 * EXBIN — public C API
 * ====================
 * A programming language designed for building programming languages and
 * tools. This header is the only one an embedder needs. It is plain C and is
 * safe to include from C++ (see also exbin.hpp for the C++ wrapper).
 *
 * Threading: one ExVM must only be used from one thread at a time.
 * GC model : the collector runs only at safepoints inside the interpreter
 *            loop (calls / loop back-edges). Values returned by the API are
 *            therefore valid until the next time the VM executes code.
 *            Store them in a global (ex_set_global) to keep them alive.
 */
#ifndef EXBIN_H
#define EXBIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXBIN_VERSION "1.0.0"
#define EXBIN_BYTECODE_VERSION 1

typedef struct ExVM ExVM;
typedef struct ExObj ExObj;

typedef enum { EX_NIL = 0, EX_BOOL = 1, EX_NUM = 2, EX_OBJ = 3 } ExType;

typedef struct ExValue {
  int type; /* ExType */
  union {
    int b;
    double n;
    ExObj* o;
  } as;
} ExValue;

typedef enum {
  EX_OK = 0,
  EX_COMPILE_ERROR = 1,
  EX_RUNTIME_ERROR = 2
} ExStatus;

/* A native function. Return 1 on success (store the result in *ret), or call
 * ex_error(...) and return 0 to raise an error. */
typedef int (*ExNative)(ExVM* vm, int argc, const ExValue* args, ExValue* ret,
                        void* ud);
typedef void (*ExWriteFn)(void* ud, const char* s, size_t n);

/* ---- lifecycle ---------------------------------------------------------- */
ExVM* ex_new(void);
void ex_free(ExVM* vm);

void ex_set_output(ExVM* vm, ExWriteFn fn, void* ud); /* redirect print() */
void ex_set_args(ExVM* vm, int argc, const char* const* argv);
void ex_add_lib_path(ExVM* vm, const char* dir); /* for require() */
void ex_set_gc_stress(ExVM* vm, int on);         /* debug: collect always */

/* ---- running code ------------------------------------------------------- */
int ex_run_string(ExVM* vm, const char* source, const char* name);
int ex_run_file(ExVM* vm, const char* path); /* source (.exb) or bytecode (.exbc) */
/* Run source and capture the value of a top-level `return`. */
int ex_eval(ExVM* vm, const char* source, const char* name, ExValue* result);
/* Last error text (message + stack trace). Valid until the next API call. */
const char* ex_error_message(ExVM* vm);

/* ---- bytecode tools ----------------------------------------------------- */
int ex_compile_to_file(ExVM* vm, const char* source, const char* name,
                       const char* out_path);
/* Returns a malloc'd listing (free with ex_free_string) or NULL on error. */
char* ex_disassemble(ExVM* vm, const char* source, const char* name);
/* Same, for a file that is either source (.exb) or bytecode (.exbc). */
char* ex_disassemble_file(ExVM* vm, const char* path);
void ex_free_string(char* s);

/* ---- natives & globals -------------------------------------------------- */
/* max_args < 0 means variadic. ud_free (optional) is called when the native
 * is garbage collected or the VM is freed. */
int ex_def_native(ExVM* vm, const char* name, ExNative fn, void* ud,
                  int min_args, int max_args, void (*ud_free)(void*));
int ex_error(ExVM* vm, const char* fmt, ...); /* always returns 0 */
int ex_get_global(ExVM* vm, const char* name, ExValue* out);
void ex_set_global(ExVM* vm, const char* name, ExValue v);
int ex_call(ExVM* vm, ExValue fn, int argc, const ExValue* args, ExValue* out);

/* ---- value construction ------------------------------------------------- */
ExValue ex_nil(void);
ExValue ex_bool(int b);
ExValue ex_num(double n);
ExValue ex_str(ExVM* vm, const char* s, size_t len);
ExValue ex_cstr(ExVM* vm, const char* s);
ExValue ex_list_new(ExVM* vm);
void ex_list_push(ExVM* vm, ExValue list, ExValue v);
ExValue ex_map_new(ExVM* vm);
void ex_map_set(ExVM* vm, ExValue map, const char* key, ExValue v);
ExValue ex_bytes_new(ExVM* vm, const void* data, size_t len);

/* ---- value inspection --------------------------------------------------- */
int ex_is_nil(ExValue v);
int ex_is_bool(ExValue v);
int ex_is_num(ExValue v);
int ex_is_string(ExValue v);
int ex_is_list(ExValue v);
int ex_is_map(ExValue v);
int ex_is_bytes(ExValue v);
int ex_is_callable(ExValue v);
int ex_truthy(ExValue v);
int ex_as_bool(ExValue v);
double ex_as_num(ExValue v);
const char* ex_as_string(ExValue v, size_t* len); /* NUL-terminated, owned by VM */
const uint8_t* ex_as_bytes(ExValue v, size_t* len);
int ex_list_len(ExValue list);
ExValue ex_list_get(ExValue list, int i);
int ex_map_len(ExValue map);
ExValue ex_map_keys(ExVM* vm, ExValue map);
int ex_map_get(ExValue map, const char* key, ExValue* out);
const char* ex_type_name(ExValue v);
ExValue ex_to_string(ExVM* vm, ExValue v); /* like str() */
ExValue ex_repr(ExVM* vm, ExValue v);      /* like repr() */

#ifdef __cplusplus
}
#endif
#endif /* EXBIN_H */
