/* EXBIN internal: the virtual machine. */
#ifndef EXBIN_VM_H
#define EXBIN_VM_H

#include "chunk.h"
#include "value.h"

#define FRAMES_MAX 1024
#define STACK_MAX (FRAMES_MAX * 256)
#define C_DEPTH_MAX 160

/* method tables are indexed by ObjType, plus these for non-objects */
#define METH_NUM 10
#define METH_BOOL 11
#define METH_NIL 12
#define METH_COUNT 13

typedef struct {
  ObjClosure* closure;
  uint8_t* ip;
  Value* slots;
} CallFrame;

struct ExVM {
  CallFrame frames[FRAMES_MAX];
  int frameCount;
  Value stack[STACK_MAX];
  Value* stackTop;

  ObjMap* globals;
  ObjMap* modules; /* require() cache: path -> result */
  ObjMap* methods[METH_COUNT]; /* x.name(...) dispatch for builtin types */
  ObjUpvalue* openUpvalues;

  /* gc */
  Obj* objects;
  size_t bytesAllocated;
  size_t nextGC;
  bool gcStress;
  Obj** gray;
  int grayCount, grayCap;

  /* errors */
  Value errval;
  bool errPositioned;
  Buf trace;
  char* errmsg;

  /* io / host */
  ExWriteFn out;
  void* outUd;
  int cdepth;
  char** libPaths;
  int libCount;
  uint64_t rng;
};

/* vm.c */
bool vm_call(VM* vm, Value fn, int argc, const Value* args, Value* out);
bool vm_error(VM* vm, const char* fmt, ...); /* sets errval, returns false */
bool vm_throw(VM* vm, Value v);              /* throw any value, returns false */
void vm_write(VM* vm, const char* s, size_t n);
void vm_push(VM* vm, Value v);
Value vm_pop(VM* vm);
void vm_reset_stack(VM* vm);
void vm_define_global(VM* vm, const char* name, Value v);
int vm_exec_function(VM* vm, ObjFunction* fn, Value* result); /* EX_* status */
void vm_set_errmsg(VM* vm, const char* msg);
char* vm_read_file(const char* path, size_t* len);

/* compiler.c */
ObjFunction* compile_source(VM* vm, const char* src, const char* file);

/* serialize.c */
bool bytecode_write(VM* vm, ObjFunction* fn, Buf* out);
ObjFunction* bytecode_read(VM* vm, const uint8_t* data, size_t len);
bool bytecode_check_magic(const char* data, size_t len);

/* natives.c / toolkit.c */
void natives_register(VM* vm);
void toolkit_register(VM* vm);
void natives_def(VM* vm, const char* name, ExNative fn, int minArgs, int maxArgs);
/* method natives get args[0]=self; min/max exclude self. `type` is a
 * value_type_name() string ("string", "list", handle kind, ...). */
void natives_def_method(VM* vm, const char* type, const char* name, ExNative fn,
                        int minArgs, int maxArgs);
ObjMap* methods_for(VM* vm, Value v, bool create);
bool vm_verror(VM* vm, const char* fmt, va_list ap);
bool deep_equal(Value a, Value b, int depth); /* structural ==, as used by the VM */
bool module_require(VM* vm, const char* path, Value* out);

#endif
