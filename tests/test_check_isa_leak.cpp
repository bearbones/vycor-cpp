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

#include "vycor/check/Analyzer.h"
#include "vycor/check/CheckSet.h"
#include "vycor/check/Checkpoint.h"
#include "vycor/check/IsaLeak.h"
#include "vycor/check/Report.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace vycor;
namespace {

// Two TUs (a.cpp, b.cpp) that include shared.h and call into it from a
// strong function each, so every inline function they reach is emitted.
struct IsaFixture {
  std::string dir;
  explicit IsaFixture(const std::string &header,
                      const std::string &uses = "") {
    llvm::SmallString<128> path;
    REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("isa_leak_test", path));
    llvm::SmallString<256> absolute;
    REQUIRE_FALSE(llvm::sys::fs::real_path(path, absolute));
    dir = std::string(absolute);
    write("shared.h", header);
    write("a.cpp", "#include \"shared.h\"\nvoid entry_a() {\n" + uses + "}\n");
    write("b.cpp", "#include \"shared.h\"\nvoid entry_b() {\n" + uses + "}\n");
    database({}, {});
  }
  ~IsaFixture() { llvm::sys::fs::remove_directories(dir); }
  void write(const std::string &name, const std::string &body) const {
    std::ofstream file(dir + "/" + name);
    REQUIRE(file.good());
    file << body;
  }
  std::vector<std::string> files() const {
    return {dir + "/a.cpp", dir + "/b.cpp"};
  }
  void database(const std::vector<std::string> &left,
                const std::vector<std::string> &right) const {
    llvm::json::Array entries;
    auto paths = files();
    for (unsigned i = 0; i < paths.size(); ++i) {
      // An explicit target keeps the feature lists the same on any host.
      llvm::json::Array args{"clang++", "-std=c++17",
                             "--target=x86_64-unknown-linux-gnu", "-I" + dir};
      for (const auto &arg : (i == 0 ? left : right))
        args.push_back(arg);
      args.push_back("-c");
      args.push_back(paths[i]);
      entries.push_back(llvm::json::Object{{"directory", dir},
                                           {"file", paths[i]},
                                           {"arguments", std::move(args)}});
    }
    write("compile_commands.json",
          llvm::formatv("{0:2}", llvm::json::Value(std::move(entries))).str());
  }
  std::unique_ptr<clang::tooling::CompilationDatabase> db() const {
    std::string error;
    auto result =
        clang::tooling::CompilationDatabase::loadFromDirectory(dir, error);
    REQUIRE(result);
    return result;
  }
};

AnalysisOptions isaOptions(unsigned facets = kIsaLeakDefaultFacets) {
  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableIsaLeakDiag = true;
  opts.isaLeakFacets = facets;
  return opts;
}

std::vector<Diagnostic> isaOnly(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::IsaLeak_Divergent ||
        d.kind == Diagnostic::IsaLeak_Inlined)
      out.push_back(d);
  return out;
}

std::vector<std::string> messages(const std::vector<Diagnostic> &all) {
  std::vector<std::string> out;
  for (const auto &d : isaOnly(all))
    out.push_back(d.callLocation + " " + d.message);
  return out;
}

bool mentions(const Diagnostic &d, const std::string &text) {
  return d.message.find(text) != std::string::npos;
}

// A loop over floats (vectorizable), straight-line integer code (not), and
// a loop-free float function (FP-sensitive, not ISA-vector-free).
const char *mixedHeader = R"cpp(
inline float dot(const float *a, const float *b, unsigned long n) {
  float s = 0;
  for (unsigned long i = 0; i < n; ++i)
    s += a[i] * b[i];
  return s;
}
inline int add_ints(int a, int b) { return a ^ b; }
inline float scale(float x) { return x * 0.5f + 1.0f; }
)cpp";
const char *mixedUses = R"cpp(
  float v[4] = {1, 2, 3, 4};
  (void)dot(v, v, 4);
  (void)add_ints(1, 2);
  (void)scale(2.0f);
)cpp";

} // namespace

TEST_CASE("isa-leak reports a vectorizable inline function built with and "
          "without -mavx2",
          "[CheckIsaLeak]") {
  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  auto opts = isaOptions();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), opts, &index));
  // dot has a loop; scale has float arithmetic (the SSE/AVX encodings
  // differ); add_ints is pure integer straight-line code.
  REQUIRE(result.size() == 2);
  const auto &d = result[0].resolvedDecl.find("dot") != std::string::npos
                      ? result[0]
                      : result[1];
  CHECK(d.checkName == "isa-leak");
  CHECK(checkNameOf(d) == "isa-leak");
  CHECK(d.kind == Diagnostic::IsaLeak_Divergent);
  CHECK(diagnosticSeverity(d) == Severity::Warning);
  CHECK(mentions(d, "+avx2"));
  CHECK(mentions(d, "a.cpp"));
  CHECK(mentions(d, "b.cpp"));
  CHECK(mentions(d, "no extra target features"));
  CHECK(mentions(d, "_Z3dotPKfS0_m"));
  CHECK(mentions(d, "a loop"));
  CHECK(d.entities.size() == 2);
  CHECK(d.entities[0] == "_Z3dotPKfS0_m");
  CHECK(d.entities[1].find("x86_64") != std::string::npos);
  for (const auto &r : result)
    CHECK(r.resolvedDecl.find("add_ints") == std::string::npos);
  // Order- and thread-independent.
  auto reversed = fixture.files();
  std::reverse(reversed.begin(), reversed.end());
  opts.threadCount = 2;
  CHECK(messages(runAnalysis(*db, reversed, opts)) == messages(result));
}

TEST_CASE("isa-leak reports floating-point functions built with differing "
          "fast-math flags",
          "[CheckIsaLeak]") {
  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"-ffast-math"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 2); // dot and scale; add_ints has no FP
  for (const auto &d : result) {
    CHECK(d.resolvedDecl.find("add_ints") == std::string::npos);
    CHECK(mentions(d, "(fp)"));
    CHECK(mentions(d, "-fassociative-math"));
    CHECK(mentions(d, "-fno-associative-math"));
  }
  // The fp facet off: nothing to compare.
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(kIsaLeakIsa)))
            .empty());
}

TEST_CASE("isa-leak is silent when both TUs are built identically",
          "[CheckIsaLeak]") {
  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"-mavx2", "-O2"}, {"-mavx2", "-O0"});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  // Both TUs did emit them: the silence is a comparison, not a gap.
  CHECK(index.isaLeakFacts().functionCount() == 3);
  CHECK(index.isaLeakFacts().sightingCount() == 6);
  // -mavx2 already implies -mavx: the effective feature sets are equal.
  fixture.database({"-mavx2"}, {"-mavx", "-mavx2"});
  db = fixture.db();
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions())).empty());
}

TEST_CASE("isa-leak excludes functions with a target attribute",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
__attribute__((target("avx2")))
inline float pinned(const float *a, unsigned long n) {
  float s = 0;
  for (unsigned long i = 0; i < n; ++i) s += a[i];
  return s;
}
)cpp",
                     "  float v[2] = {1, 2};\n"
                     "  if (__builtin_cpu_supports(\"avx2\"))\n"
                     "    (void)pinned(v, 2);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  CHECK(index.isaLeakFacts().functionCount() == 1); // collected, then skipped
}

TEST_CASE("isa-leak ignores inline functions no emitted code reaches",
          "[CheckIsaLeak]") {
  // helper is referenced only by an inline function nothing calls: no TU
  // emits either, whatever Decl::isUsed says.
  IsaFixture fixture(R"cpp(
inline float helper(const float *a, unsigned long n) {
  float s = 0;
  for (unsigned long i = 0; i < n; ++i) s += a[i];
  return s;
}
inline float unused(const float *a) { return helper(a, 4); }
inline float reached(const float *a, unsigned long n) {
  float s = 1;
  for (unsigned long i = 0; i < n; ++i) s *= a[i];
  return s;
}
)cpp",
                     "  float v[2] = {1, 2};\n  (void)reached(v, 2);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl.find("reached") != std::string::npos);
}

TEST_CASE("isa-leak folds what a body can inline into its sensitivity",
          "[CheckIsaLeak]") {
  // outer has no loop of its own, but inlines inner's.
  IsaFixture fixture(R"cpp(
inline unsigned inner(const unsigned *a, unsigned long n) {
  unsigned s = 0;
  for (unsigned long i = 0; i < n; ++i) s += a[i];
  return s;
}
inline bool outer(const unsigned *a) { return inner(a, 4) != 0; }
)cpp",
                     "  unsigned v[4] = {};\n  (void)outer(v);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 2);
  for (const auto &d : result) {
    const bool outer = d.resolvedDecl.find("outer") != std::string::npos;
    // outer has no loop of its own: a lead (note), not a finding.
    CHECK(d.kind == (outer ? Diagnostic::IsaLeak_Inlined
                           : Diagnostic::IsaLeak_Divergent));
    CHECK(diagnosticSeverity(d) ==
          (outer ? Severity::Note : Severity::Warning));
    CHECK(checkNameOf(d) == "isa-leak");
    CHECK(mentions(d, "only through functions it can inline") == outer);
  }
}

TEST_CASE("isa-leak reports cleanup around throwing calls under differing "
          "exception modes",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
struct Guard { ~Guard(); };
void may_throw();
inline void guarded() { Guard g; may_throw(); }
inline int plain(int x) { return x ^ 1; }
)cpp",
                     "  guarded();\n  (void)plain(1);\n");
  fixture.database({"-fno-exceptions"}, {"-fexceptions"});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl.find("guarded") != std::string::npos);
  CHECK(mentions(result[0], "-fno-exceptions"));
  CHECK(mentions(result[0], "skips its destructors"));
}

TEST_CASE("isa-leak signed-overflow facet is opt-in", "[CheckIsaLeak]") {
  IsaFixture fixture("inline int sum(int a, int b) { return a + b; }\n",
                     "  (void)sum(1, 2);\n");
  fixture.database({"-fwrapv"}, {});
  auto db = fixture.db();
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions())).empty());
  auto result = isaOnly(runAnalysis(
      *db, fixture.files(), isaOptions(kIsaLeakDefaultFacets | kIsaLeakOverflow)));
  REQUIRE(result.size() == 1);
  CHECK(mentions(result[0], "-fwrapv"));
}

TEST_CASE("isa-leak partitions by target triple and skips system headers",
          "[CheckIsaLeak]") {
  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"--target=i386-unknown-linux-gnu", "-mavx2"}, {});
  auto db = fixture.db();
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions())).empty());
  // The same header as a system header: out of scope by default.
  fixture.write("a.cpp", "#include <shared.h>\nvoid entry_a() {\n" +
                             std::string(mixedUses) + "}\n");
  fixture.write("b.cpp", "#include <shared.h>\nvoid entry_b() {\n" +
                             std::string(mixedUses) + "}\n");
  fixture.database({"-isystem" + fixture.dir, "-mavx2"},
                   {"-isystem" + fixture.dir});
  db = fixture.db();
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions())).empty());
}

TEST_CASE("isa-leak reports template instantiations emitted by both TUs",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
template <typename T> T total(const T *a, unsigned long n) {
  T s = 0;
  for (unsigned long i = 0; i < n; ++i) s += a[i];
  return s;
}
)cpp",
                     "  double v[2] = {1, 2};\n  (void)total(v, 2);\n");
  fixture.database({"-march=haswell"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl.find("total<double>") != std::string::npos);
  CHECK(mentions(result[0], "more")); // haswell adds many features
}

TEST_CASE("isa-leak facts survive absorb and worker shards but not the "
          "phase-2 handoff",
          "[CheckIsaLeak]") {
  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"-mavx2"}, {"-ffast-math"});
  auto db = fixture.db();
  GlobalIndex original;
  auto expected =
      messages(runAnalysis(*db, fixture.files(), isaOptions(), &original));
  REQUIRE(expected.size() == 2);
  GlobalIndex merged;
  merged.absorb(original);
  CHECK(merged.isaLeakFacts().functionCount() ==
        original.isaLeakFacts().functionCount());
  CHECK(merged.isaLeakFacts().profiles().size() == 2);
  std::vector<Diagnostic> result;
  analyzeIsaLeaks(merged, kIsaLeakDefaultFacets, result);
  CHECK(messages(result) == expected);

  const auto shard = fixture.dir + "/worker.bin";
  REQUIRE(writeCheckIndexShard(
      shard, {{"a.cpp", CheckIndexPayload::capture(original)}},
      {{TuStatus::Indexed, ""}}));
  GlobalIndex replayed;
  REQUIRE(readCheckIndexShard(shard, [&](const auto &, const auto &payload) {
    payload.applyTo(replayed);
  }));
  result.clear();
  analyzeIsaLeaks(replayed, kIsaLeakDefaultFacets, result);
  CHECK(messages(result) == expected);

  // Phase-2 workers never read the facts: the handoff leaves them out.
  const auto handoff = fixture.dir + "/handoff.bin";
  REQUIRE(writeGlobalIndexFile(handoff, original));
  GlobalIndex loaded;
  REQUIRE(readGlobalIndexFile(handoff, loaded));
  CHECK(loaded.isaLeakFacts().functionCount() == 0);
}

TEST_CASE("isa-leak is opt-in with a validated facet list", "[CheckIsaLeak]") {
  auto opts = isaOptions();
  const auto enabled = checkOptionsFingerprint(opts);
  opts.enableIsaLeakDiag = false;
  CHECK(checkOptionsFingerprint(opts) != enabled);
  CHECK_FALSE(defaultCheckSet().count("isa-leak"));
  CHECK(checkSeverity("isa-leak") == Severity::Warning);
  std::set<std::string> checks;
  std::string error;
  REQUIRE(resolveCheckSpec({"-all", "isa-leak"}, checks, error));
  CHECK(checks == std::set<std::string>{"isa-leak"});

  unsigned facets = 0;
  REQUIRE(parseIsaLeakFacets("isa, fp", facets, error));
  CHECK(facets == (kIsaLeakIsa | kIsaLeakFp));
  REQUIRE(parseIsaLeakFacets("default,overflow", facets, error));
  CHECK(facets == (kIsaLeakDefaultFacets | kIsaLeakOverflow));
  REQUIRE(parseIsaLeakFacets("all", facets, error));
  CHECK(facets == (kIsaLeakDefaultFacets | kIsaLeakOverflow));
  facets = 7;
  CHECK_FALSE(parseIsaLeakFacets("isa,simd", facets, error));
  CHECK(error.find("simd") != std::string::npos);
  CHECK(facets == 7);
  CHECK_FALSE(parseIsaLeakFacets("", facets, error));

  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), opts, &index)).empty());
  CHECK(index.isaLeakFacts().functionCount() == 0); // nothing collected
}

TEST_CASE("isa-leak counts math-errno only for bodies with math calls",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
inline float arith(float x) { return x * 0.5f; }
inline double root(double x) { return __builtin_sqrt(x); }
)cpp",
                     "  (void)arith(1.0f);\n  (void)root(2.0);\n");
  fixture.database({"-fno-math-errno"}, {"-fmath-errno"});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl.find("root") != std::string::npos);
  CHECK(mentions(result[0], "-fno-math-errno"));
  CHECK(mentions(result[0], "math-library calls"));
  CHECK_FALSE(mentions(result[0], "target attribute"));
}

TEST_CASE("isa-leak does not count a virtual call as emitting the callee",
          "[CheckIsaLeak]") {
  // Shape's vtable is homed in shape.cpp (key function), not selected
  // here: a.cpp and b.cpp only call area() through the vtable, so neither
  // emits it.
  IsaFixture fixture(R"cpp(
struct Shape {
  virtual ~Shape();
  virtual float area(const float *a, unsigned long n) const {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
)cpp",
                     "  extern Shape *shape;\n  float v[2] = {};\n"
                     "  (void)shape->area(v, 2);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  CHECK(index.isaLeakFacts().functionCount() == 0);
}

TEST_CASE("isa-leak folds bits through a long inline chain", "[CheckIsaLeak]") {
  // The loop at the bottom reaches the top through 80 inlinable levels.
  std::string header = "inline float f0(const float *a) { float s = 0; "
                       "for (int i = 0; i < 4; ++i) s += a[i]; return s; }\n";
  for (int i = 1; i <= 80; ++i)
    header += "inline int f" + std::to_string(i) + "(const float *a);\n";
  for (int i = 80; i >= 1; --i)
    header += "inline int f" + std::to_string(i) +
              "(const float *a) { return f" + std::to_string(i - 1) +
              "(a) > 0; }\n";
  IsaFixture fixture(header, "  float v[4] = {};\n  (void)f80(v);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  CHECK(result.size() == 81);
}

TEST_CASE("isa-leak does not treat do-while-zero macros as loops",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
void fail(const char *);
#define CHECK_THAT(x) do { if (!(x)) fail(#x); } while (0)
struct Buffer {
  unsigned char *data;
  unsigned long step;
  unsigned char *row(int y) { CHECK_THAT(y >= 0); return data + step * y; }
  const unsigned char *row(int y) const { CHECK_THAT(y >= 0); return data + step * y; }
  unsigned long spin(unsigned long n) { while (n > 1) n /= 2; return n; }
};
)cpp",
                     "  Buffer b{};\n  const Buffer &c = b;\n"
                     "  (void)b.row(0);\n  (void)c.row(0);\n"
                     "  (void)b.spin(9);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  auto result =
      isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl.find("spin") != std::string::npos);
  CHECK(mentions(result[0], "including what it can inline"));
  // Both overloads were collected, under distinct display names.
  std::set<std::string> names;
  for (const auto &r : index.isaLeakFacts().records())
    names.insert(r.qualifiedName);
  CHECK(names.count("Buffer::row(int)"));
  CHECK(names.count("Buffer::row(int) const"));
}

TEST_CASE("isa-leak counts floating-point values that are only moved",
          "[CheckIsaLeak]") {
  // No arithmetic, but the value travels in an XMM register: VEX-encoded
  // under -mavx.
  IsaFixture fixture(R"cpp(
float narrow(double x);
struct Pair { double re, im; };
inline float first(const Pair &p) { return narrow(p.re); }
)cpp",
                     "  Pair p{1, 2};\n  (void)first(p);\n");
  fixture.database({"-mavx"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].kind == Diagnostic::IsaLeak_Divergent);
}
