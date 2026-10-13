/* EXBIN internal: values, heap objects, buffers. */
#ifndef EXBIN_VALUE_H
#define EXBIN_VALUE_H

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exbin.h"

typedef ExValue Value;
typedef struct ExVM VM;

/* ------------------------------------------------------------------ Buf -- */
typedef struct {
  char* d;
  size_t len, cap;
} Buf;

void buf_init(Buf* b);
void buf_free(Buf* b);
void buf_add(Buf* b, const char* s, size_t n);
void buf_addc(Buf* b, char c);
void buf_adds(Buf* b, const char* s);
void buf_addf(Buf* b, const char* fmt, ...);
const char* buf_str(Buf* b); /* NUL-terminated view */
char* buf_take(Buf* b);      /* malloc'd NUL-terminated, buffer reset */

/* ---------------------------------------------------------------- values -- */
#define NIL_VAL ((Value){EX_NIL, {.n = 0}})
#define BOOL_VAL(v) ((Value){EX_BOOL, {.b = (v) ? 1 : 0}})
#define NUM_VAL(v) ((Value){EX_NUM, {.n = (v)}})
#define OBJ_VAL(ptr) ((Value){EX_OBJ, {.o = (Obj*)(ptr)}})

#define IS_NIL(v) ((v).type == EX_NIL)
#define IS_BOOL(v) ((v).type == EX_BOOL)
#define IS_NUM(v) ((v).type == EX_NUM)
#define IS_OBJ(v) ((v).type == EX_OBJ)
#define AS_BOOL(v) ((v).as.b)
#define AS_NUM(v) ((v).as.n)
#define AS_OBJ(v) ((v).as.o)

typedef enum {
  OBJ_STRING,
  OBJ_LIST,
  OBJ_MAP,
  OBJ_BYTES,
  OBJ_RANGE,
  OBJ_FUNCTION,
  OBJ_CLOSURE,
  OBJ_UPVALUE,
  OBJ_NATIVE,
  OBJ_HANDLE
} ObjType;

struct ExObj {
  ObjType type;
  bool marked;
  struct ExObj* next;
};
typedef struct ExObj Obj;

typedef struct {
  Value* v;
  int count, cap;
} ValueArray;

typedef struct {
  uint8_t* code;
  int count, cap;
  int* lines;
  ValueArray consts;
} Chunk;

typedef struct {
  Obj obj;
  int len;
  uint32_t hash;
  char chars[];
} ObjString;

typedef struct {
  Obj obj;
  int count, cap;
  Value* items;
} ObjList;

typedef struct {
  Value key, value; /* key == nil => deleted entry */
} MapEntry;

/* Insertion-ordered hash map (like Python's dict). */
typedef struct {
  Obj obj;
  int count; /* live entries */
  int used;  /* entry slots used (live + deleted) */
  int cap;   /* entry capacity */
  MapEntry* entries;
  int* index; /* open addressing: -1 empty, -2 tombstone, else entry index */
  int icap;
} ObjMap;

typedef struct {
  Obj obj;
  int len, cap;
  uint8_t* data;
} ObjBytes;

typedef struct {
  Obj obj;
  double start, stop, step;
} ObjRange;

typedef struct ObjFunction {
  Obj obj;
  int arity;
  int upvalueCount;
  int maxStack; /* computed by chunk_verify */
  Chunk chunk;
  ObjString* name;
  ObjString* file;
} ObjFunction;

typedef struct ObjUpvalue {
  Obj obj;
  Value* location;
  Value closed;
  struct ObjUpvalue* next;
} ObjUpvalue;

typedef struct {
  Obj obj;
  ObjFunction* fn;
  ObjUpvalue** upvalues;
  int upvalueCount;
} ObjClosure;

typedef struct {
  Obj obj;
  ExNative fn;
  void* ud;
  void (*udFree)(void*);
  int minArgs, maxArgs; /* maxArgs < 0 => variadic */
  char* name;
} ObjNative;

/* Opaque host object (lexers, regexes, assemblers...). */
typedef struct {
  Obj obj;
  const char* kind;
  void* data;
  void (*mark)(VM* vm, void* data);
  void (*destroy)(void* data);
} ObjHandle;

#define OBJ_TYPE(v) (AS_OBJ(v)->type)
#define IS_OBJ_TYPE(v, t) (IS_OBJ(v) && AS_OBJ(v)->type == (t))
#define IS_STRING(v) IS_OBJ_TYPE(v, OBJ_STRING)
#define IS_LIST(v) IS_OBJ_TYPE(v, OBJ_LIST)
#define IS_MAP(v) IS_OBJ_TYPE(v, OBJ_MAP)
#define IS_BYTES(v) IS_OBJ_TYPE(v, OBJ_BYTES)
#define IS_RANGE(v) IS_OBJ_TYPE(v, OBJ_RANGE)
#define IS_FUNCTION(v) IS_OBJ_TYPE(v, OBJ_FUNCTION)
#define IS_CLOSURE(v) IS_OBJ_TYPE(v, OBJ_CLOSURE)
#define IS_NATIVE(v) IS_OBJ_TYPE(v, OBJ_NATIVE)
#define IS_HANDLE(v) IS_OBJ_TYPE(v, OBJ_HANDLE)
#define IS_CALLABLE(v) (IS_FUNCTION(v) || IS_CLOSURE(v) || IS_NATIVE(v))

#define AS_STRING(v) ((ObjString*)AS_OBJ(v))
#define AS_CSTRING(v) (AS_STRING(v)->chars)
#define AS_LIST(v) ((ObjList*)AS_OBJ(v))
#define AS_MAP(v) ((ObjMap*)AS_OBJ(v))
#define AS_BYTES(v) ((ObjBytes*)AS_OBJ(v))
#define AS_RANGE(v) ((ObjRange*)AS_OBJ(v))
#define AS_FUNCTION(v) ((ObjFunction*)AS_OBJ(v))
#define AS_CLOSURE(v) ((ObjClosure*)AS_OBJ(v))
#define AS_NATIVE(v) ((ObjNative*)AS_OBJ(v))
#define AS_HANDLE(v) ((ObjHandle*)AS_OBJ(v))

/* ---------------------------------------------------------------- memory -- */
void* ex_realloc(VM* vm, void* p, size_t oldSize, size_t newSize);
#define MEM_ALLOC(vm, T, n) ((T*)ex_realloc((vm), NULL, 0, sizeof(T) * (size_t)(n)))
#define MEM_FREE(vm, T, p, n) ex_realloc((vm), (p), sizeof(T) * (size_t)(n), 0)
#define MEM_GROW(vm, T, p, old, nw) \
  ((T*)ex_realloc((vm), (p), sizeof(T) * (size_t)(old), sizeof(T) * (size_t)(nw)))

/* ------------------------------------------------------------- objects --- */
ObjString* str_copy(VM* vm, const char* s, size_t len);
ObjString* str_from(VM* vm, const char* cstr);
ObjString* str_concat(VM* vm, ObjString* a, ObjString* b);
uint32_t hash_bytes(const char* s, size_t len);

ObjList* list_new(VM* vm);
void list_push(VM* vm, ObjList* l, Value v);
void list_insert(VM* vm, ObjList* l, int idx, Value v);
Value list_remove(ObjList* l, int idx);

ObjMap* map_new(VM* vm);
bool map_get(ObjMap* m, Value key, Value* out);
bool map_get_str(VM* vm, ObjMap* m, const char* key, Value* out);
bool map_set(VM* vm, ObjMap* m, Value key, Value v); /* true if new key */
void map_set_str(VM* vm, ObjMap* m, const char* key, Value v);
bool map_delete(ObjMap* m, Value key);

ObjBytes* bytes_new(VM* vm, int len);
void bytes_push(VM* vm, ObjBytes* b, uint8_t byte);
ObjRange* range_new(VM* vm, double start, double stop, double step);
int64_t range_len(const ObjRange* r);

ObjFunction* fn_new(VM* vm);
ObjClosure* closure_new(VM* vm, ObjFunction* fn);
ObjUpvalue* upvalue_new(VM* vm, Value* slot);
ObjNative* native_new(VM* vm, const char* name, ExNative fn, void* ud,
                      int minArgs, int maxArgs, void (*udFree)(void*));
ObjHandle* handle_new(VM* vm, const char* kind, void* data,
                      void (*mark)(VM*, void*), void (*destroy)(void*));

bool values_equal(Value a, Value b);
uint32_t hash_value(Value v);
bool value_truthy(Value v);
const char* value_type_name(Value v);

void fmt_number(Buf* b, double n);
void value_tostr_buf(VM* vm, Buf* b, Value v); /* like str() */
void value_repr_buf(VM* vm, Buf* b, Value v, int depth);
ObjString* value_tostring(VM* vm, Value v);
ObjString* value_repr(VM* vm, Value v);

/* ------------------------------------------------------------------- gc -- */
void gc_mark_value(VM* vm, Value v);
void gc_mark_obj(VM* vm, Obj* o);
void gc_collect(VM* vm);
void gc_free_all(VM* vm);

/* chunk helpers (chunk.c) */
void chunk_init(Chunk* c);
void chunk_free(VM* vm, Chunk* c);
void chunk_write(VM* vm, Chunk* c, uint8_t byte, int line);
int chunk_add_const(VM* vm, Chunk* c, Value v);

#endif
