// embed.cpp — using EXBIN from C++ through exbin.hpp
#include <iostream>
#include <string>
#include <vector>

#include "exbin.hpp"

int main() {
  exbin::VM vm;
  std::string captured;
  vm.setOutput([&](const std::string& s) { captured += s; });  // redirect print()

  vm.defNative("shout", 1, 1, [](exbin::VM& vm, const std::vector<exbin::Value>& a, exbin::Value& ret) {
    if (!a[0].isString()) return vm.error("shout: string expected");
    std::string s = a[0].asString();
    for (auto& c : s) c = (char)toupper((unsigned char)c);
    ret = exbin::Value::string(vm, s + "!");
    return true;
  });

  if (vm.run("print(shout('hello'), 1 + 2)", "embed.cpp") != exbin::Status::Ok) {
    std::cerr << vm.errorMessage();
    return 1;
  }
  std::cout << "captured output: " << captured;

  exbin::Value v;
  vm.eval("return {items: [1, 2, 3], ok: true}", "eval", &v);
  exbin::Value items;
  if (!v.get("items", items) || items.listSize() != 3) return 2;
  std::cout << "items[2] = " << items.at(2).asNumber() << "\n";
  std::cout << "repr: " << v.repr(vm) << "\n";

  if (vm.run("error('from script')", "bad") == exbin::Status::RuntimeError)
    std::cout << "runtime error text: " << vm.errorMessage();

  vm.reset();  // fresh state
  exbin::Value missing;
  return vm.getGlobal("shout", missing) ? 3 : 0;  // gone after reset
}
