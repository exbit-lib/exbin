// EXBIN — C++17 wrapper around the C API (exbin.h).
//
//   exbin::VM vm;
//   vm.defNative("add", 2, 2, [](exbin::VM& vm, const std::vector<exbin::Value>& a, exbin::Value& ret) {
//     ret = exbin::Value::number(a[0].asNumber() + a[1].asNumber());
//     return true;
//   });
//   vm.run("print(add(2, 3))");
//
// Values are only valid until the VM next executes code (see exbin.h).
#ifndef EXBIN_HPP
#define EXBIN_HPP

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "exbin.h"

namespace exbin {

enum class Status { Ok = EX_OK, CompileError = EX_COMPILE_ERROR, RuntimeError = EX_RUNTIME_ERROR };

class VM;

class Value {
 public:
  Value() : v_(ex_nil()) {}
  explicit Value(ExValue v) : v_(v) {}
  static Value nil() { return Value(); }
  static Value boolean(bool b) { return Value(ex_bool(b)); }
  static Value number(double n) { return Value(ex_num(n)); }
  static Value string(VM& vm, const std::string& s);
  static Value list(VM& vm);
  static Value map(VM& vm);

  bool isNil() const { return ex_is_nil(v_); }
  bool isBool() const { return ex_is_bool(v_); }
  bool isNumber() const { return ex_is_num(v_); }
  bool isString() const { return ex_is_string(v_); }
  bool isList() const { return ex_is_list(v_); }
  bool isMap() const { return ex_is_map(v_); }
  bool isBytes() const { return ex_is_bytes(v_); }
  bool isCallable() const { return ex_is_callable(v_); }
  bool truthy() const { return ex_truthy(v_) != 0; }
  bool asBool() const { return ex_as_bool(v_) != 0; }
  double asNumber() const { return ex_as_num(v_); }
  std::string asString() const {
    size_t n = 0;
    const char* s = ex_as_string(v_, &n);
    return s ? std::string(s, n) : std::string();
  }
  std::vector<uint8_t> asBytes() const {
    size_t n = 0;
    const uint8_t* p = ex_as_bytes(v_, &n);
    return p ? std::vector<uint8_t>(p, p + n) : std::vector<uint8_t>();
  }
  const char* typeName() const { return ex_type_name(v_); }
  int listSize() const { return ex_list_len(v_); }
  Value at(int i) const { return Value(ex_list_get(v_, i)); }
  int mapSize() const { return ex_map_len(v_); }
  bool get(const std::string& key, Value& out) const {
    ExValue r;
    if (!ex_map_get(v_, key.c_str(), &r)) return false;
    out = Value(r);
    return true;
  }
  inline std::string str(VM& vm) const;
  inline std::string repr(VM& vm) const;
  ExValue raw() const { return v_; }

 private:
  ExValue v_;
};

class VM {
 public:
  using Native = std::function<bool(VM&, const std::vector<Value>&, Value&)>;
  using Output = std::function<void(const std::string&)>;

  VM() { init(); }
  ~VM() { ex_free(vm_); }
  VM(const VM&) = delete;
  VM& operator=(const VM&) = delete;

  ExVM* raw() { return vm_; }

  void addLibPath(const std::string& dir) {
    libs_.push_back(dir);
    ex_add_lib_path(vm_, dir.c_str());
  }
  void setArgs(const std::vector<std::string>& args) {
    args_ = args;
    applyArgs();
  }
  void setOutput(Output fn) {
    out_ = std::make_shared<Output>(std::move(fn));
    applyOutput();
  }
  void setGcStress(bool on) { ex_set_gc_stress(vm_, on); }

  Status run(const std::string& src, const std::string& name = "<string>") {
    return Status(ex_run_string(vm_, src.c_str(), name.c_str()));
  }
  Status runFile(const std::string& path) { return Status(ex_run_file(vm_, path.c_str())); }
  Status eval(const std::string& src, const std::string& name, Value* result = nullptr) {
    ExValue r = ex_nil();
    Status st = Status(ex_eval(vm_, src.c_str(), name.c_str(), &r));
    if (result) *result = Value(r);
    return st;
  }
  Status compileToFile(const std::string& src, const std::string& name, const std::string& out) {
    return Status(ex_compile_to_file(vm_, src.c_str(), name.c_str(), out.c_str()));
  }
  bool disassemble(const std::string& src, const std::string& name, std::string& listing) {
    char* s = ex_disassemble(vm_, src.c_str(), name.c_str());
    if (!s) return false;
    listing = s;
    ex_free_string(s);
    return true;
  }
  bool disassembleFile(const std::string& path, std::string& listing) {
    char* s = ex_disassemble_file(vm_, path.c_str());
    if (!s) return false;
    listing = s;
    ex_free_string(s);
    return true;
  }
  std::string errorMessage() { return ex_error_message(vm_); }
  // Raise an error from inside a native; always returns false.
  bool error(const std::string& msg) { return ex_error(vm_, "%s", msg.c_str()) != 0; }

  void defNative(const std::string& name, int minArgs, int maxArgs, Native fn) {
    auto* ctx = new Ctx{this, std::move(fn)};
    ex_def_native(vm_, name.c_str(), &trampoline, ctx, minArgs, maxArgs, &freeCtx);
  }
  bool getGlobal(const std::string& name, Value& out) {
    ExValue r;
    if (!ex_get_global(vm_, name.c_str(), &r)) return false;
    out = Value(r);
    return true;
  }
  void setGlobal(const std::string& name, Value v) { ex_set_global(vm_, name.c_str(), v.raw()); }
  bool call(Value fn, const std::vector<Value>& args, Value& out) {
    std::vector<ExValue> raw;
    for (auto& a : args) raw.push_back(a.raw());
    ExValue r = ex_nil();
    bool ok = ex_call(vm_, fn.raw(), (int)raw.size(), raw.data(), &r) != 0;
    out = Value(r);
    return ok;
  }

  // Throw away all state (globals, modules) and start fresh.
  void reset() {
    ex_free(vm_);
    init();
    for (auto& l : libs_) ex_add_lib_path(vm_, l.c_str());
    applyArgs();
    applyOutput();
  }

 private:
  struct Ctx {
    VM* vm;
    Native fn;
  };
  static int trampoline(ExVM*, int argc, const ExValue* args, ExValue* ret, void* ud) {
    Ctx* c = static_cast<Ctx*>(ud);
    std::vector<Value> a;
    a.reserve((size_t)argc);
    for (int i = 0; i < argc; i++) a.emplace_back(args[i]);
    Value r;
    if (!c->fn(*c->vm, a, r)) return 0;
    *ret = r.raw();
    return 1;
  }
  static void freeCtx(void* p) { delete static_cast<Ctx*>(p); }
  static void writer(void* ud, const char* s, size_t n) { (*static_cast<Output*>(ud))(std::string(s, n)); }
  void init() { vm_ = ex_new(); }
  void applyArgs() {
    std::vector<const char*> p;
    for (auto& a : args_) p.push_back(a.c_str());
    ex_set_args(vm_, (int)p.size(), p.data());
  }
  void applyOutput() {
    if (out_) ex_set_output(vm_, &writer, out_.get());
    else ex_set_output(vm_, nullptr, nullptr);
  }

  ExVM* vm_ = nullptr;
  std::vector<std::string> libs_, args_;
  std::shared_ptr<Output> out_;
};

inline Value Value::string(VM& vm, const std::string& s) { return Value(ex_str(vm.raw(), s.data(), s.size())); }
inline Value Value::list(VM& vm) { return Value(ex_list_new(vm.raw())); }
inline Value Value::map(VM& vm) { return Value(ex_map_new(vm.raw())); }
inline std::string Value::str(VM& vm) const { return Value(ex_to_string(vm.raw(), v_)).asString(); }
inline std::string Value::repr(VM& vm) const { return Value(ex_repr(vm.raw(), v_)).asString(); }

}  // namespace exbin

#endif  // EXBIN_HPP
