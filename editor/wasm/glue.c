// WebAssembly glue for the EXBIN editor.
#include <stdlib.h>
#include <string.h>
#include "exbin.h"

static char*  g_out = NULL;
static size_t g_len = 0, g_cap = 0;

static void sink(void* ud, const char* s, size_t n) {
  (void)ud;
  if (g_len + n + 1 > g_cap) {
    size_t nc = g_cap ? g_cap : 4096;
    while (nc < g_len + n + 1) nc *= 2;
    g_out = (char*)realloc(g_out, nc);
    g_cap = nc;
  }
  memcpy(g_out + g_len, s, n);
  g_len += n;
  g_out[g_len] = 0;
}

static void reset(void) { g_len = 0; if (g_out) g_out[0] = 0; }

/* Returns status (0 ok, 1 compile error, 2 runtime error). Output via exb_out(). */
__attribute__((export_name("exb_run")))
int exb_run(const char* src) {
  reset();
  ExVM* vm = ex_new();
  ex_set_output(vm, sink, NULL);
  ex_add_lib_path(vm, "/lib");
  { const char* av[1] = {"main.exb"}; ex_set_args(vm, 0, av); }
  int st = ex_run_string(vm, src, "main.exb");
  if (st != EX_OK) { const char* m = ex_error_message(vm); if (m) sink(NULL, m, strlen(m)); }
  ex_free(vm);
  return st;
}

/* Disassembly listing; returns 0 on success, 1 on error. */
__attribute__((export_name("exb_disasm")))
int exb_disasm(const char* src) {
  reset();
  ExVM* vm = ex_new();
  ex_add_lib_path(vm, "/lib");
  char* d = ex_disassemble(vm, src, "main.exb");
  int rc = 0;
  if (d) { sink(NULL, d, strlen(d)); ex_free_string(d); }
  else { const char* m = ex_error_message(vm); if (m) sink(NULL, m, strlen(m)); rc = 1; }
  ex_free(vm);
  return rc;
}

__attribute__((export_name("exb_out")))  const char* exb_out(void) { return g_out ? g_out : ""; }
__attribute__((export_name("exb_len")))  int exb_len(void) { return (int)g_len; }
__attribute__((export_name("exb_alloc"))) void* exb_alloc(int n) { return malloc((size_t)n); }
__attribute__((export_name("exb_free")))  void exb_free(void* p) { free(p); }
__attribute__((export_name("exb_version"))) const char* exb_version(void) { return EXBIN_VERSION; }
