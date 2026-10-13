// EXBIN command line driver (C++17): script runner, REPL, bytecode tools.
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "exbin.hpp"

static void usage() {
  std::puts(
      "EXBIN " EXBIN_VERSION " - a language for building languages and tools\n"
      "\n"
      "usage:\n"
      "  exbin                      start the interactive REPL\n"
      "  exbin <file> [args...]     run a source (.exb) or bytecode (.exbc) file\n"
      "  exbin -e <code>            run a snippet\n"
      "  exbin -c <file> [-o out]   compile source to bytecode (default: <file>c)\n"
      "  exbin -d <file>            disassemble a source or bytecode file\n"
      "  exbin -L <dir>             add a require() search directory (repeatable)\n"
      "  exbin -v | --version       print the version\n"
      "  exbin -h | --help          this text");
}

static bool read_all(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  char buf[65536];
  size_t n;
  out.clear();
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}

static int repl(exbin::VM& vm) {
  std::printf("EXBIN %s  (type .help, Ctrl-D to quit)\n", EXBIN_VERSION);
  std::string buffer;
  for (;;) {
    std::fputs(buffer.empty() ? ">> " : ".. ", stdout);
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) {
      std::puts("");
      return 0;
    }
    if (buffer.empty() && line == ".help") {
      std::puts("  .exit        leave the REPL\n  .reset       forget all definitions\n"
                "  an expression is printed; end a line with \\ to continue it");
      continue;
    }
    if (buffer.empty() && (line == ".exit" || line == ".quit")) return 0;
    if (buffer.empty() && line == ".reset") {
      vm.reset();
      continue;
    }
    if (!line.empty() && line.back() == '\\') {
      line.pop_back();
      buffer += line + "\n";
      continue;
    }
    buffer += line;
    std::string src = buffer;
    buffer.clear();
    if (src.find_first_not_of(" \t\r\n") == std::string::npos) continue;
    // First try it as an expression whose value we print; fall back to a statement.
    exbin::Value result;
    exbin::Status st = vm.eval("return " + src, "<repl>", &result);
    if (st == exbin::Status::CompileError) st = vm.eval(src, "<repl>", &result);
    if (st == exbin::Status::Ok) {
      if (!result.isNil()) std::printf("%s\n", result.repr(vm).c_str());
    } else {
      std::fputs(vm.errorMessage().c_str(), stderr);
    }
  }
}

int main(int argc, char** argv) {
  std::vector<std::string> libs;
  std::string mode = "run", file, code, outPath;
  std::vector<std::string> scriptArgs;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (!file.empty() && mode == "run") {
      scriptArgs.push_back(a);
      continue;
    }
    if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else if (a == "-v" || a == "--version") {
      std::puts("EXBIN " EXBIN_VERSION);
      return 0;
    } else if (a == "-e" && i + 1 < argc) {
      mode = "eval";
      code = argv[++i];
    } else if (a == "-c" && i + 1 < argc) {
      mode = "compile";
      file = argv[++i];
    } else if (a == "-d" && i + 1 < argc) {
      mode = "disasm";
      file = argv[++i];
    } else if (a == "-o" && i + 1 < argc) {
      outPath = argv[++i];
    } else if (a == "-L" && i + 1 < argc) {
      libs.push_back(argv[++i]);
    } else if (!a.empty() && a[0] == '-' && a != "-") {
      std::fprintf(stderr, "exbin: unknown option '%s' (try --help)\n", a.c_str());
      return 64;
    } else {
      file = a;
    }
  }

  exbin::VM vm;
  if (std::getenv("EXBIN_GC_STRESS")) vm.setGcStress(true);
  if (const char* env = std::getenv("EXBIN_PATH")) libs.push_back(env);
  for (auto& l : libs) vm.addLibPath(l);
  std::vector<std::string> all;
  all.push_back(file.empty() ? "exbin" : file);
  all.insert(all.end(), scriptArgs.begin(), scriptArgs.end());
  vm.setArgs(all);

  if (mode == "eval") {
    exbin::Status st = vm.run(code, "<-e>");
    if (st != exbin::Status::Ok) {
      std::fputs(vm.errorMessage().c_str(), stderr);
      return st == exbin::Status::CompileError ? 65 : 70;
    }
    return 0;
  }
  if (mode == "compile") {
    std::string src;
    if (!read_all(file, src)) {
      std::fprintf(stderr, "exbin: cannot read '%s'\n", file.c_str());
      return 66;
    }
    if (outPath.empty()) outPath = file + "c";
    if (vm.compileToFile(src, file, outPath) != exbin::Status::Ok) {
      std::fputs(vm.errorMessage().c_str(), stderr);
      return 65;
    }
    std::printf("wrote %s\n", outPath.c_str());
    return 0;
  }
  if (mode == "disasm") {
    std::string listing;
    if (!vm.disassembleFile(file, listing)) {
      std::fputs(vm.errorMessage().c_str(), stderr);
      return 65;
    }
    std::fputs(listing.c_str(), stdout);
    return 0;
  }
  if (file.empty()) return repl(vm);
  exbin::Status st = vm.runFile(file);
  if (st != exbin::Status::Ok) {
    std::fputs(vm.errorMessage().c_str(), stderr);
    return st == exbin::Status::CompileError ? 65 : 70;
  }
  return 0;
}
