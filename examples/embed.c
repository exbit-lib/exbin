/* embed.c — using EXBIN from plain C through exbin.h */
#include <stdio.h>
#include <string.h>

#include "exbin.h"

/* A native: hypot(a, b) */
static int native_hypot(ExVM* vm, int argc, const ExValue* args, ExValue* ret, void* ud) {
  (void)argc;
  (void)ud;
  if (!ex_is_num(args[0]) || !ex_is_num(args[1])) return ex_error(vm, "hypot: numbers expected");
  double a = ex_as_num(args[0]), b = ex_as_num(args[1]);
  double s = a * a + b * b, r = s;
  for (int i = 0; i < 40 && r > 0; i++) r = (r + s / r) / 2; /* Newton */
  *ret = ex_num(r);
  return 1;
}

int main(void) {
  ExVM* vm = ex_new();
  ex_def_native(vm, "hypot", native_hypot, NULL, 2, 2, NULL);

  const char* script =
      "fn area(w, h) { return w * h }\n"
      "let config = {name: \"demo\", sizes: [1, 2, 3]}\n"
      "print(\"hypot(3, 4) =\", hypot(3, 4))\n";
  if (ex_run_string(vm, script, "embed.c") != EX_OK) {
    fprintf(stderr, "%s", ex_error_message(vm));
    return 1;
  }

  /* call an EXBIN function from C */
  ExValue area, result;
  if (!ex_get_global(vm, "area", &area)) return 2;
  ExValue args[2] = {ex_num(6), ex_num(7)};
  if (!ex_call(vm, area, 2, args, &result)) {
    fprintf(stderr, "%s", ex_error_message(vm));
    return 3;
  }
  printf("area(6, 7) from C = %g\n", ex_as_num(result));

  /* read data back */
  ExValue config, name;
  ex_get_global(vm, "config", &config);
  ex_map_get(config, "name", &name);
  size_t n;
  const char* s = ex_as_string(name, &n);
  printf("config.name = %.*s\n", (int)n, s);

  /* evaluate an expression */
  ExValue v;
  if (ex_eval(vm, "return [1, 2, 3].map(fn(x) { return x * 10 }).sum()", "eval", &v) == EX_OK)
    printf("eval -> %g\n", ex_as_num(v));

  /* errors are reported, not fatal */
  if (ex_run_string(vm, "return nil + 1", "bad.exb") != EX_OK) printf("caught: %s", ex_error_message(vm));

  int ok = ex_as_num(result) == 42;
  ex_free(vm);
  return ok ? 0 : 4;
}
