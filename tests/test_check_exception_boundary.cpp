// Copyright (c) 2026 The vycor-cpp Authors
// Original author: Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// exception-boundary-escape (docs/checks/exception-boundary-escape.md):
// each boundary kind, the caught and typed-handler paths, what is left to
// exception-escape, and the facts' trip through worker shards.

#include "vycor/check/Analyzer.h"
#include "vycor/check/CheckSet.h"
#include "vycor/check/Checkpoint.h"
#include "vycor/check/Report.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace vycor;
namespace {

struct Source {
  std::string name;
  std::string body;
  std::vector<std::string> extraArgs = {};
};
using Sources = std::vector<Source>;

// .cpp files are C++ TUs and .c files C TUs, each with its own extra
// flags; anything else is a header beside them.
struct BoundaryFixture {
  std::string dir;
  std::vector<std::string> sources;

  explicit BoundaryFixture(const Sources &files) {
    llvm::SmallString<128> path;
    REQUIRE_FALSE(
        llvm::sys::fs::createUniqueDirectory("exception_boundary_test", path));
    llvm::SmallString<256> absolute;
    REQUIRE_FALSE(llvm::sys::fs::real_path(path, absolute));
    dir = std::string(absolute);
    llvm::json::Array entries;
    for (const Source &file : files) {
      write(file.name, file.body);
      const bool cpp = llvm::StringRef(file.name).ends_with(".cpp");
      const bool c = llvm::StringRef(file.name).ends_with(".c");
      if (!cpp && !c)
        continue;
      sources.push_back(dir + "/" + file.name);
      llvm::json::Array args;
      args.push_back(cpp ? "clang++" : "clang");
      args.push_back(cpp ? "-std=c++17" : "-std=c11");
      args.push_back("-I" + dir);
      for (const std::string &extra : file.extraArgs)
        args.push_back(extra);
      args.push_back("-c");
      args.push_back(sources.back());
      entries.push_back(llvm::json::Object{{"directory", dir},
                                           {"file", sources.back()},
                                           {"arguments", std::move(args)}});
    }
    write("compile_commands.json",
          llvm::formatv("{0:2}", llvm::json::Value(std::move(entries))).str());
  }
  ~BoundaryFixture() { llvm::sys::fs::remove_directories(dir); }

  void write(const std::string &name, const std::string &body) const {
    std::ofstream file(dir + "/" + name);
    REQUIRE(file.good());
    file << body;
  }
  std::unique_ptr<clang::tooling::CompilationDatabase> db() const {
    std::string error;
    auto result =
        clang::tooling::CompilationDatabase::loadFromDirectory(dir, error);
    REQUIRE(result);
    return result;
  }
};

AnalysisOptions boundaryOptions() {
  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableExceptionBoundaryDiag = true;
  return opts;
}

std::vector<Diagnostic> boundariesOnly(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::Custom &&
        d.checkName == "exception-boundary-escape")
      out.push_back(d);
  return out;
}

// Findings by "<kind> <function>", the boundary kind being the first
// fingerprint entity.
std::map<std::string, Diagnostic>
byBoundary(const std::vector<Diagnostic> &all) {
  std::map<std::string, Diagnostic> out;
  for (const auto &d : boundariesOnly(all)) {
    REQUIRE(d.entities.size() >= 2);
    out.emplace(d.entities[0] + " " + d.entities[1], d);
  }
  return out;
}

std::map<std::string, Diagnostic> run(const Sources &files,
                                      GlobalIndex *index = nullptr) {
  BoundaryFixture fixture(files);
  auto db = fixture.db();
  return byBoundary(
      runAnalysis(*db, fixture.sources, boundaryOptions(), index));
}

bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

std::set<std::string> keysOf(const std::map<std::string, Diagnostic> &m) {
  std::set<std::string> out;
  for (const auto &[key, diag] : m)
    out.insert(key);
  return out;
}

const char *kThrower = "#include <stdexcept>\n"
                       "inline void parse(int x) {\n"
                       "  if (x) throw std::runtime_error(\"bad\");\n"
                       "}\n";

} // namespace

TEST_CASE("exception boundary escape: std::thread entries that reach a throw",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"a.cpp", "#include \"t.h\"\n"
                "#include <thread>\n"
                "#include <vector>\n"
                "void work() { parse(1); }\n"
                "void caught() { try { parse(1); } catch (...) {} }\n"
                "void typed() {\n"
                "  try { parse(1); } catch (const std::logic_error &) {}\n"
                "}\n"
                "void quiet() noexcept { parse(1); }\n"
                "void startAll() {\n"
                "  std::thread a(work);\n"
                "  std::thread b([] { caught(); });\n"
                "  std::thread c(&typed);\n"
                "  std::thread d(quiet);\n"
                "  a.join(); b.join(); c.join(); d.join();\n"
                "}\n"
                "void pooled() { parse(2); }\n"
                "void pool(std::vector<std::thread> &workers) {\n"
                "  workers.emplace_back(pooled);\n"
                "  workers.emplace(workers.begin(), [] { parse(3); });\n"
                "}\n"},
  });
  // caught() has a catch (...) around the throw; quiet() is noexcept, so
  // it is exception-escape's.
  // A thread built in place in a container is handed over at the
  // emplace call.
  CHECK(keysOf(found) ==
        std::set<std::string>{"thread-entry lambda in 'pool'",
                              "thread-entry pooled", "thread-entry typed",
                              "thread-entry work"});

  const Diagnostic &work = found.at("thread-entry work");
  CHECK_FALSE(work.note);
  CHECK(work.rank == 3); // after the two pool entries, by name
  CHECK(contains(work.callLocation, "a.cpp:4:6"));
  CHECK(contains(work.message, "started by std::thread at"));
  CHECK(contains(work.message, "Witness: 'work' -> 'parse'; 'parse' throws "
                               "at "));
  CHECK(contains(work.message, "t.h:3"));
  CHECK(contains(work.message, "Confidence: proven"));
  CHECK(work.entities ==
        std::vector<std::string>{"thread-entry", "work", "std::thread"});

  // Only a try whose handlers are all typed stands between: a note.
  const Diagnostic &typed = found.at("thread-entry typed");
  CHECK(typed.note);
  CHECK(typed.rank == 4);
  CHECK(contains(typed.message, "Confidence: typed-handlers"));
}

TEST_CASE("exception boundary escape: catch (...) that rethrows lets the "
          "try block's exceptions out",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"a.cpp", "#include \"t.h\"\n"
                "#include <map>\n"
                "#include <thread>\n"
                "void cleanup();\n"
                "void rethrows() {\n"
                "  try { parse(1); } catch (...) { cleanup(); throw; }\n"
                "}\n"
                "// The node constructor libstdc++ wraps in a rethrowing\n"
                "// catch (...) cannot throw for an int key and value.\n"
                "void fills(std::map<int, int> &m) { m.emplace(1, 2); }\n"
                "void go(std::map<int, int> &m) {\n"
                "  std::thread a(rethrows), b(fills, std::ref(m));\n"
                "  a.join(); b.join();\n"
                "}\n"},
  });
  CHECK(keysOf(found) == std::set<std::string>{"thread-entry rethrows"});
  const Diagnostic &d = found.at("thread-entry rethrows");
  CHECK_FALSE(d.note);
  CHECK(contains(d.message, "'rethrows' -> 'parse'"));
}

TEST_CASE("exception boundary escape: a lambda thread entry is summarized "
          "where it is written",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"a.cpp", "#include \"t.h\"\n"
                "#include <thread>\n"
                "void go() {\n"
                "  std::thread t([] { parse(2); });\n"
                "  t.join();\n"
                "}\n"},
  });
  REQUIRE(found.count("thread-entry lambda in 'go'"));
  const Diagnostic &d = found.at("thread-entry lambda in 'go'");
  CHECK_FALSE(d.note);
  CHECK(contains(d.message, "'lambda in 'go'' -> 'parse'"));
}

TEST_CASE("exception boundary escape: pthread_create and thrd_create start "
          "routines are thread entries",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"a.cpp",
       "#include \"t.h\"\n"
       "extern \"C\" int pthread_create(void *, const void *,\n"
       "                                void *(*)(void *), void *);\n"
       "extern \"C\" int thrd_create(void *, int (*)(void *), void *);\n"
       "void *start(void *) { parse(1); return nullptr; }\n"
       "int start11(void *) { parse(1); return 0; }\n"
       "void spawn() {\n"
       "  pthread_create(nullptr, nullptr, start, nullptr);\n"
       "  thrd_create(nullptr, start11, nullptr);\n"
       "}\n"},
  });
  CHECK(keysOf(found) == std::set<std::string>{"thread-entry start",
                                               "thread-entry start11"});
  CHECK(contains(found.at("thread-entry start").message,
                 "started by pthread_create at"));
  CHECK(contains(found.at("thread-entry start11").message,
                 "started by thrd_create at"));
}

TEST_CASE("exception boundary escape: a callback needs a C receiver",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"a.cpp",
       "#include \"t.h\"\n"
       "#include <cstdlib>\n"
       "extern \"C\" void register_cb(void (*)(int));\n"
       "extern \"C\" void cpp_receiver(void (*cb)(int)) noexcept {\n"
       "  try { cb(1); } catch (...) {}\n"
       "}\n"
       "void cxx_receiver(void (*cb)(int));\n"
       "int cmp(const void *a, const void *) {\n"
       "  parse(*static_cast<const int *>(a));\n"
       "  return 0;\n"
       "}\n"
       "void onEvent(int x) { parse(x); }\n"
       "void viaCpp(int x) { parse(x); }\n"
       "void viaCxx(int x) { parse(x); }\n"
       "void setup(int *v) {\n"
       "  qsort(v, 2, sizeof(int), cmp);\n"
       "  register_cb(onEvent);\n"
       "  register_cb(&onEvent);\n"
       "  cpp_receiver(viaCpp);\n"
       "  cxx_receiver(viaCxx);\n"
       "}\n"},
  });
  // cpp_receiver is C++ compiled with exceptions: the callback unwinds
  // into it, and its own extern "C" definition is the boundary (here
  // noexcept and caught, so nothing). cxx_receiver has C++ linkage.
  CHECK(keysOf(found) ==
        std::set<std::string>{"c-callback cmp", "c-callback onEvent"});
  const Diagnostic &cmp = found.at("c-callback cmp");
  CHECK_FALSE(cmp.note);
  CHECK(contains(cmp.message, "passed as a callback to the C function "
                              "'qsort'"));
  const Diagnostic &onEvent = found.at("c-callback onEvent");
  CHECK(contains(onEvent.message, "and 1 other site"));
  CHECK(onEvent.entities ==
        std::vector<std::string>{"c-callback", "onEvent", "register_cb"});
}

TEST_CASE("exception boundary escape: function-pointer fields of a C struct",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"hooks.h", "#ifdef __cplusplus\nextern \"C\" {\n#endif\n"
                  "struct hooks { int version; void (*on_event)(int); };\n"
                  "#ifdef __cplusplus\n}\n#endif\n"
                  "struct cxx_hooks { void (*on_event)(int); };\n"},
      {"a.cpp", "#include \"t.h\"\n"
                "#include \"hooks.h\"\n"
                "void assigned(int x) { parse(x); }\n"
                "void initialized(int x) { parse(x); }\n"
                "void notC(int x) { parse(x); }\n"
                "void install(hooks *h) {\n"
                "  h->on_event = assigned;\n"
                "  hooks local = {1, initialized};\n"
                "  cxx_hooks other = {notC};\n"
                "  (void)local; (void)other;\n"
                "}\n"},
  });
  CHECK(keysOf(found) == std::set<std::string>{"c-callback assigned",
                                               "c-callback initialized"});
  CHECK(contains(found.at("c-callback assigned").message,
                 "stored in the function-pointer field 'hooks::on_event' of "
                 "a C struct"));
}

TEST_CASE("exception boundary escape: extern \"C\" definitions",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"api.cpp", "#include \"t.h\"\n"
                  "void inner(int x) { parse(x); }\n"
                  "extern \"C\" void api_throws(int x) { inner(x); }\n"
                  "extern \"C\" void api_caught(int x) {\n"
                  "  try { inner(x); } catch (...) {}\n"
                  "}\n"
                  "extern \"C\" void api_noexcept(int x) noexcept { inner(x); }\n"
                  "extern \"C\" int api_plain(int x) { return x + 1; }\n"},
  });
  CHECK(keysOf(found) == std::set<std::string>{"extern-c api_throws"});
  const Diagnostic &d = found.at("extern-c api_throws");
  CHECK_FALSE(d.note);
  CHECK(contains(d.message, "Witness: 'api_throws' -> 'inner' -> 'parse'"));
  CHECK_FALSE(contains(d.message, "compiled without exceptions"));
}

TEST_CASE("exception boundary escape: frames compiled without exceptions",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"api.cpp", "#include \"t.h\"\n"
                  "void inner(int x) { parse(x); }\n"
                  "extern \"C\" void api_throws(int x) { inner(x); }\n"
                  "void cxx_throws(int x) { inner(x); }\n"
                  "int cxx_quiet(int x) { return x; }\n"},
      {"user.c", "void api_throws(int);\n"
                 "void c_user(void) { api_throws(1); }\n"},
      {"noeh.cpp",
       "void cxx_throws(int);\n"
       "int cxx_quiet(int);\n"
       "void noeh_user() { cxx_throws(1); }\n"
       "int noeh_quiet() { return cxx_quiet(1); }\n",
       {"-fno-exceptions"}},
  });
  // The C caller of an extern "C" function is named on that function's
  // finding rather than reported on its own.
  CHECK(keysOf(found) ==
        std::set<std::string>{"extern-c api_throws",
                              "no-exceptions-frame noeh_user"});
  CHECK(contains(found.at("extern-c api_throws").message,
                 "called from code compiled without exceptions, such as "
                 "'c_user'"));
  const Diagnostic &frame = found.at("no-exceptions-frame noeh_user");
  CHECK_FALSE(frame.note);
  CHECK(contains(frame.callLocation, "noeh.cpp:3:6"));
  CHECK(contains(frame.message,
                 "Witness: 'noeh_user' -> 'cxx_throws' -> 'inner' -> "
                 "'parse'"));
}

TEST_CASE("exception boundary escape: a callee with no definition is an "
          "unknown witness unless it is C",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"a.cpp", "#include <thread>\n"
                "void elsewhere();\n"
                "extern \"C\" void c_elsewhere(void);\n"
                "void callsCxx() { elsewhere(); }\n"
                "void callsC() { c_elsewhere(); }\n"
                "struct Base { virtual void run(); };\n"
                "void dispatch(Base *b) { b->run(); }\n"
                "void go(Base *b) {\n"
                "  std::thread x(callsCxx), y(callsC), z(dispatch, b);\n"
                "  x.join(); y.join(); z.join();\n"
                "}\n"},
  });
  CHECK(keysOf(found) == std::set<std::string>{"thread-entry callsCxx",
                                               "thread-entry dispatch"});
  const Diagnostic &cxx = found.at("thread-entry callsCxx");
  CHECK(cxx.note);
  CHECK(contains(cxx.message, "'elsewhere()' has no definition in the "
                              "index"));
  CHECK(contains(cxx.message, "Confidence: unknown"));
  CHECK(contains(found.at("thread-entry dispatch").message,
                 "makes a virtual or indirect call that may throw"));
}

TEST_CASE("exception boundary escape: a handed-over function nothing "
          "defines is its own witness",
          "[CheckExceptionBoundary]") {
  // flushA is also called directly, so it is a node of the graph with no
  // fact; flushB is known only from the hand-over.
  auto found = run({
      {"a.cpp", "#include <thread>\n"
                "void flushA();\n"
                "void flushB();\n"
                "void direct() { flushA(); }\n"
                "void go() {\n"
                "  std::thread a(flushA), b(flushB);\n"
                "  a.join(); b.join();\n"
                "}\n"},
  });
  CHECK(keysOf(found) == std::set<std::string>{"thread-entry flushA",
                                               "thread-entry flushB"});
  for (const char *name : {"flushA", "flushB"}) {
    const Diagnostic &d = found.at(std::string("thread-entry ") + name);
    CHECK(d.note);
    CHECK(contains(d.message, std::string("Witness: '") + name + "'; '" +
                                  name + "' has no definition in the "
                                         "index."));
    CHECK(contains(d.message, "Confidence: unknown"));
  }
}

TEST_CASE("exception boundary escape: a handler that rethrows stops nothing "
          "and raises nothing new",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"t.h", kThrower},
      {"a.cpp", "#include \"t.h\"\n"
                "#include <thread>\n"
                "void quiet() {}\n"
                "void typedQuiet() {\n"
                "  try { quiet(); } catch (const std::exception &) { throw; }\n"
                "}\n"
                "void typedLoud() {\n"
                "  try { parse(1); } catch (const std::exception &) { throw; }\n"
                "}\n"
                "void typedBeforeAll() {\n"
                "  try { parse(1); }\n"
                "  catch (const std::logic_error &) { throw; }\n"
                "  catch (...) {}\n"
                "}\n"
                "struct Ctor {\n"
                "  int v;\n"
                "  Ctor() try : v(1) {} catch (...) { throw; }\n"
                "};\n"
                "void makes() { Ctor c; (void)c; }\n"
                "void go() {\n"
                "  std::thread a(typedQuiet), b(typedLoud), c(typedBeforeAll),\n"
                "      d(makes);\n"
                "  a.join(); b.join(); c.join(); d.join();\n"
                "}\n"},
  });
  CHECK(keysOf(found) ==
        std::set<std::string>{"thread-entry typedLoud",
                              "thread-entry typedBeforeAll"});
  const Diagnostic &loud = found.at("thread-entry typedLoud");
  CHECK_FALSE(loud.note);
  CHECK(contains(loud.message, "'typedLoud' -> 'parse'"));
  const Diagnostic &before = found.at("thread-entry typedBeforeAll");
  CHECK(before.note);
  CHECK(contains(before.message, "Confidence: typed-handlers"));
}

TEST_CASE("exception boundary escape: library throws and unknown calls "
          "behind typed handlers",
          "[CheckExceptionBoundary]") {
  auto found = run({
      {"a.cpp", "#include <stdexcept>\n"
                "#include <thread>\n"
                "#include <vector>\n"
                "int third(std::vector<int> *v) { return v->at(3); }\n"
                "struct Base { virtual void run(); };\n"
                "void guarded(Base *b) {\n"
                "  try { b->run(); } catch (const std::exception &) {}\n"
                "}\n"
                "void elsewhere();\n"
                "void guardedCall() {\n"
                "  try { elsewhere(); } catch (int) {}\n"
                "}\n"
                "// Allocation is no witness, behind a typed try or not.\n"
                "int allocs(int n) {\n"
                "  try {\n"
                "    std::vector<int> v(n);\n"
                "    int *p = new int(n);\n"
                "    int r = *p + int(v.size());\n"
                "    delete p;\n"
                "    return r;\n"
                "  } catch (const std::exception &) { return 0; }\n"
                "}\n"
                "void go(std::vector<int> *v, Base *b) {\n"
                "  std::thread x(third, v), y(guarded, b), z(guardedCall),\n"
                "      w(allocs, 3);\n"
                "  x.join(); y.join(); z.join(); w.join();\n"
                "}\n"},
  });
  CHECK(keysOf(found) ==
        std::set<std::string>{"thread-entry third", "thread-entry guarded",
                              "thread-entry guardedCall"});
  const Diagnostic &lib = found.at("thread-entry third");
  CHECK(lib.note);
  CHECK(contains(lib.message, "reaches a throw in the standard library"));
  CHECK(contains(lib.message, "Confidence: library"));
  const Diagnostic &virt = found.at("thread-entry guarded");
  CHECK(contains(virt.message, "in a try whose handlers are all typed"));
  CHECK(contains(virt.message, "Confidence: unknown"));
  const Diagnostic &call = found.at("thread-entry guardedCall");
  CHECK(contains(call.message, "'elsewhere()' has no definition"));
  CHECK(contains(call.message, "Confidence: unknown"));
  // Ranked by class first: library before unknown.
  CHECK(lib.rank < virt.rank);
  CHECK(lib.rank < call.rank);
}

TEST_CASE("exception boundary escape: one hand-over seen from two TUs "
          "merges the same in either order",
          "[CheckExceptionBoundary]") {
  ExceptionBoundaryEntry a;
  a.kind = ExceptionBoundaryEntry::ThreadEntry;
  a.target = "u:lambda";
  a.qualifiedName = "lambda in 'go'";
  a.filePath = "/p/h.h";
  a.line = 3;
  a.column = 5;
  a.api = "std::thread";
  a.apiSite = "/p/h.h:3:5";
  a.reasons = NoexceptFactEntry::OtherBare;
  a.reasonSite = "/p/h.h:4";
  a.callees = {"m:_Z1fv"};
  a.calleeBits = {NoexceptFactEntry::Exit | NoexceptFactEntry::Bare};
  ExceptionBoundaryEntry b = a;
  b.reasons = NoexceptFactEntry::ThrowBare;
  b.reasonSite = "/p/h.h:6";
  b.callees = {"m:_Z1fv", "m:_Z1gv"};
  b.calleeBits = {NoexceptFactEntry::Call, NoexceptFactEntry::Exit};
  auto merged = [](std::vector<ExceptionBoundaryEntry> order) {
    GlobalIndex index;
    for (auto &e : order) {
      std::vector<ExceptionBoundaryEntry> one;
      one.push_back(std::move(e));
      index.addExceptionBoundaries(std::move(one));
    }
    std::vector<ExceptionBoundaryEntry> out;
    index.forEachExceptionBoundary(
        [&](const ExceptionBoundaryEntry &e) { out.push_back(e); });
    return out;
  };
  const auto ab = merged({a, b});
  const auto ba = merged({b, a});
  REQUIRE(ab.size() == 1);
  REQUIRE(ba.size() == 1);
  for (const auto *e : {&ab[0], &ba[0]}) {
    CHECK(e->reasons ==
          (NoexceptFactEntry::OtherBare | NoexceptFactEntry::ThrowBare));
    CHECK(e->reasonSite == "/p/h.h:6");
    CHECK(e->callees == std::vector<std::string>{"m:_Z1fv", "m:_Z1gv"});
    CHECK(e->calleeBits ==
          std::vector<uint8_t>{static_cast<uint8_t>(
                                   NoexceptFactEntry::Exit |
                                   NoexceptFactEntry::Bare |
                                   NoexceptFactEntry::Call),
                               NoexceptFactEntry::Exit});
  }
}

TEST_CASE("exception boundary escape: findings survive absorb and worker "
          "shards",
          "[CheckExceptionBoundary]") {
  BoundaryFixture fixture({
      {"t.h", kThrower},
      {"a.cpp", "#include \"t.h\"\n"
                "#include <thread>\n"
                "extern \"C\" void api(int x) { parse(x); }\n"
                "void go() {\n"
                "  std::thread t([] { parse(3); });\n"
                "  t.join();\n"
                "}\n"},
  });
  auto db = fixture.db();
  GlobalIndex original;
  auto render = [](const std::vector<Diagnostic> &all) {
    std::vector<std::string> out;
    for (const auto &d : boundariesOnly(all))
      out.push_back(std::to_string(d.rank) + " " + d.callLocation + " " +
                    d.message);
    return out;
  };
  auto expected = render(
      runAnalysis(*db, fixture.sources, boundaryOptions(), &original));
  REQUIRE(expected.size() == 2);
  CHECK(original.exceptionBoundaryCount() == 1);
  auto analyze = [](const GlobalIndex &index) {
    std::vector<Diagnostic> out;
    analyzeExceptionBoundaries(index, {}, out);
    return out;
  };

  GlobalIndex merged;
  merged.absorb(original);
  merged.absorb(original);
  CHECK(merged.exceptionBoundaryCount() == 1);
  CHECK(render(analyze(merged)) == expected);

  const auto shard = fixture.dir + "/worker.bin";
  REQUIRE(writeCheckIndexShard(
      shard, {{"a.cpp", CheckIndexPayload::capture(original)}},
      {{TuStatus::Indexed, ""}}));
  GlobalIndex replayed;
  REQUIRE(readCheckIndexShard(shard, [&](const auto &, const auto &payload) {
    payload.applyTo(replayed);
  }));
  CHECK(replayed.exceptionBoundaryCount() == 1);
  CHECK(render(analyze(replayed)) == expected);
}

TEST_CASE("exception boundary escape: the check is opt-in and noisy",
          "[CheckExceptionBoundary]") {
  CHECK_FALSE(defaultCheckSet().count("exception-boundary-escape"));
  CHECK(checkSeverity("exception-boundary-escape") == Severity::Warning);
  CHECK_FALSE(checkHelpUri("exception-boundary-escape").empty());
  std::set<std::string> checks;
  std::string error;
  REQUIRE(resolveCheckSpec({"-all", "noisy"}, checks, error));
  CHECK(checks.count("exception-boundary-escape"));

  AnalysisOptions opts;
  const auto off = checkOptionsFingerprint(opts);
  opts.enableExceptionBoundaryDiag = true;
  CHECK(off != checkOptionsFingerprint(opts));
  CHECK(opts.collectNoexceptFacts());
  CHECK_FALSE(opts.enableNoexceptCandidates());

  // The check alone reports no noexcept candidates.
  BoundaryFixture fixture({{"a.cpp", "int f() { return 0; }\n"}});
  auto db = fixture.db();
  AnalysisOptions only = boundaryOptions();
  for (const Diagnostic &d : runAnalysis(*db, fixture.sources, only))
    CHECK(d.kind == Diagnostic::Custom);
}
