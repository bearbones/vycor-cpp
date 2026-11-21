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
    // Only what FP flags change is named: not dot's loop.
    CHECK(mentions(d, "floating-point arithmetic"));
    CHECK_FALSE(mentions(d, "a loop"));
    CHECK_FALSE(mentions(d, "target features"));
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
  // A CPU name and a feature it already implies.
  fixture.database({"-march=haswell"}, {"-march=haswell", "-mavx2", "-mfma"});
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
  CHECK(index.isaLeakFacts().functionCount() == 0); // never a sighting
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
  fixture.database({"-ftrapv"}, {});
  db = fixture.db();
  result = isaOnly(runAnalysis(
      *db, fixture.files(), isaOptions(kIsaLeakDefaultFacets | kIsaLeakOverflow)));
  REQUIRE(result.size() == 1);
  CHECK(mentions(result[0], "-ftrapv"));
  CHECK(mentions(result[0], "signed overflow undefined"));
}

TEST_CASE("isa-leak partitions by target triple and skips system headers",
          "[CheckIsaLeak]") {
  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"--target=i386-unknown-linux-gnu", "-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  // Both TUs emitted all three: the silence is the partition.
  CHECK(index.isaLeakFacts().sightingCount() == 6);
  CHECK(index.isaLeakFacts().profiles().size() == 2);
  // The same header as a system header: out of scope by default.
  fixture.write("a.cpp", "#include <shared.h>\nvoid entry_a() {\n" +
                             std::string(mixedUses) + "}\n");
  fixture.write("b.cpp", "#include <shared.h>\nvoid entry_b() {\n" +
                             std::string(mixedUses) + "}\n");
  fixture.database({"-isystem" + fixture.dir, "-mavx2"},
                   {"-isystem" + fixture.dir});
  db = fixture.db();
  GlobalIndex system;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &system))
            .empty());
  CHECK(system.isaLeakFacts().functionCount() == 0);
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
  // On a complete object the call is direct: both TUs emit area.
  fixture.write("a.cpp", "#include \"shared.h\"\nextern Shape make();\n"
                         "float entry_a(const float *v) { return make().area(v, 2); }\n");
  fixture.write("b.cpp", "#include \"shared.h\"\nextern Shape &shape;\n"
                         "float entry_b(const float *v) {\n"
                         "  Shape copy = shape; return copy.area(v, 2); }\n");
  db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl.find("Shape::area") != std::string::npos);
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
    names.insert(r.function.qualifiedName);
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
  CHECK(mentions(result[0], "floating-point values"));
  CHECK(mentions(result[0], "+avx)"));
  // Both copies are VEX-encoded: only avx changes how values move.
  fixture.database({"-mavx2", "-mfma"}, {"-mavx"});
  db = fixture.db();
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions())).empty());
  // FP flags change arithmetic, not moves: -ffast-math leaves it alone.
  fixture.database({"-ffast-math"}, {});
  db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  CHECK(index.isaLeakFacts().functionCount() == 1);
}

TEST_CASE("isa-leak leaves by-value parameters to the caller in the Itanium "
          "ABI",
          "[CheckIsaLeak]") {
  // The caller destroys `o` (Itanium): take has no cleanup of its own, so
  // the exception mode does not change its code.
  IsaFixture fixture(R"cpp(
struct Owner { Owner(); Owner(const Owner &); ~Owner(); int v; };
void may_throw();
inline void take(Owner o) { may_throw(); }
)cpp",
                     "  Owner o;\n  take(o);\n");
  fixture.database({"-fno-exceptions"}, {"-fexceptions"});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  CHECK(index.isaLeakFacts().functionCount() == 1);
  // The Microsoft ABI destroys it in the callee: a cleanup around a call
  // that may throw.
  fixture.database({"--target=x86_64-pc-windows-msvc", "-fno-exceptions"},
                   {"--target=x86_64-pc-windows-msvc", "-fexceptions",
                    "-fcxx-exceptions"});
  db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(mentions(result[0], "-fno-exceptions"));
}

TEST_CASE("isa-leak destroys trivial_abi parameters in the callee",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
struct [[clang::trivial_abi]] Handle { Handle(); Handle(const Handle &); ~Handle(); int v; };
void may_throw();
inline void take(Handle h) { may_throw(); }
)cpp",
                     "  Handle h;\n  take(h);\n");
  fixture.database({"-fno-exceptions"}, {"-fexceptions"});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl == "take(Handle)");
}

TEST_CASE("isa-leak skips code the compiler evaluates or discards",
          "[CheckIsaLeak]") {
  // sum is only ever constant-evaluated or unevaluated: no TU emits it.
  IsaFixture fixture(R"cpp(
constexpr int sum(int n) { int s = 0; for (int i = 0; i < n; ++i) s += i; return s; }
inline int use(int x) {
  constexpr int k = sum(4);
  static_assert(sum(3) == 3, "");
  switch (x) { case sum(2): return 1; default: break; }
  return k + int(sizeof(sum(2))) + int(noexcept(sum(1)));
}
template <bool B> inline int pick() {
  if constexpr (B) return sum(5); else return 0;
}
)cpp",
                     "  (void)use(1);\n  (void)pick<false>();\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  std::set<std::string> names;
  for (const auto &r : index.isaLeakFacts().records())
    names.insert(r.function.qualifiedName);
  CHECK(names.count("use(int)"));
  CHECK(names.count("pick<false>()"));
  CHECK_FALSE(names.count("sum(int)"));
}

TEST_CASE("isa-leak skips requires expressions and if consteval branches",
          "[CheckIsaLeak]") {
  // C++23 forms of unevaluated or constant-only code: sum stays unemitted.
  IsaFixture fixture(R"cpp(
constexpr int sum(int n) { int s = 0; for (int i = 0; i < n; ++i) s += i; return s; }
constexpr int twice(int x) {
  if consteval { return sum(x); } else { return 2 * x; }
}
inline int use(int x) {
  constexpr bool ok = requires { sum(1); };
  int g = _Generic(sum(2), int: 1, default: 0);
  decltype(sum(3)) d = 0;
  return twice(x) + int(ok) + g + d;
}
)cpp",
                     "  (void)use(1);\n");
  fixture.database({"-std=c++23", "-mavx2"}, {"-std=c++23"});
  auto db = fixture.db();
  GlobalIndex index;
  (void)runAnalysis(*db, fixture.files(), isaOptions(), &index);
  std::set<std::string> names;
  for (const auto &r : index.isaLeakFacts().records())
    names.insert(r.function.qualifiedName);
  CHECK(names.count("use(int)"));
  CHECK(names.count("twice(int)"));
  CHECK_FALSE(names.count("sum(int)"));
}

TEST_CASE("isa-leak emits the inherited slots of a constructed vtable",
          "[CheckIsaLeak]") {
  // Base's vtable is homed elsewhere (key function anchor). Derived has no
  // key function, so constructing it emits its vtable here, and with it
  // Base::sum, which Derived inherits without overriding.
  IsaFixture fixture(R"cpp(
struct Base {
  virtual void anchor();
  virtual float sum(const float *a, unsigned long n) const {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
struct Derived : Base { void anchor() override {} };
)cpp",
                     "  Derived d;\n  (void)&d;\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl == "Base::sum(const float *, unsigned long) const");
}

TEST_CASE("isa-leak emits the virtual functions of a vtable the TU homes",
          "[CheckIsaLeak]") {
  // a.cpp defines Homed's key function, so it emits the vtable and every
  // virtual function in it; b.cpp only names the class.
  IsaFixture fixture(R"cpp(
struct Homed {
  virtual void key();
  virtual float sum(const float *a, unsigned long n) const {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
)cpp");
  fixture.write("a.cpp", "#include \"shared.h\"\nvoid Homed::key() {}\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  const auto &records = index.isaLeakFacts().records();
  REQUIRE(records.size() == 1);
  CHECK(records[0].function.qualifiedName ==
        "Homed::sum(const float *, unsigned long) const");
  REQUIRE(records[0].sightings.size() == 1);
  CHECK(index.isaLeakFacts().tuPath(records[0].sightings[0].second) ==
        fixture.dir + "/a.cpp");
}

TEST_CASE("isa-leak emits a global variable's initializer only when it runs",
          "[CheckIsaLeak]") {
  // fill and eagerInit run in dynamic initializers of every TU (a strong
  // variable; an inline one whose initializer has side effects); folded
  // only in a constant one; lazyInit only where Holder<int>::value is
  // referenced and so instantiated.
  IsaFixture fixture(R"cpp(
inline float fill(unsigned long n) {
  float s = 0;
  for (unsigned long i = 0; i < n; ++i) s += 1.5f;
  return s;
}
inline float eagerInit(unsigned long n) {
  float s = 1;
  for (unsigned long i = 0; i < n; ++i) s *= 2.0f;
  return s;
}
constexpr int folded(int n) { int s = 0; for (int i = 0; i < n; ++i) s += i; return s; }
inline float lazyInit(unsigned long n) {
  float s = 1;
  for (unsigned long i = 0; i < n; ++i) s -= 0.5f;
  return s;
}
float runs = fill(4);
inline float eager = eagerInit(3);
inline int constant = folded(3);
template <typename T> struct Holder { static float value; };
template <typename T> float Holder<T>::value = lazyInit(3);
)cpp");
  fixture.write("b.cpp", "#include \"shared.h\"\n"
                         "float entry_b() { return Holder<int>::value; }\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  // Two strong definitions of `runs` would not link, but each TU is
  // parsed on its own: both emit fill.
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index));
  std::set<std::string> reported;
  for (const auto &d : result)
    reported.insert(d.resolvedDecl);
  CHECK(reported == std::set<std::string>{"eagerInit(unsigned long)",
                                          "fill(unsigned long)"});
  std::set<std::string> names;
  for (const auto &r : index.isaLeakFacts().records()) {
    names.insert(r.function.qualifiedName);
    if (r.function.qualifiedName == "lazyInit(unsigned long)") {
      REQUIRE(r.sightings.size() == 1);
      CHECK(index.isaLeakFacts().tuPath(r.sightings[0].second) ==
            fixture.dir + "/b.cpp");
    }
  }
  CHECK_FALSE(names.count("folded(int)"));
  CHECK(names.count("lazyInit(unsigned long)"));
}

namespace {
CodegenProfile x86Profile(std::vector<std::string> features) {
  CodegenProfile p;
  p.triple = "x86_64-unknown-linux-gnu";
  p.features = std::move(features);
  p.exceptions = true;
  return p;
}
EmittedFunction loopFunction() {
  EmittedFunction f;
  f.linkageName = "_Z4loopv";
  f.qualifiedName = "loop()";
  f.filePath = "/p/shared.h";
  f.line = 3;
  f.bodyBits = kBodyLoop;
  f.ownBits = kBodyLoop;
  return f;
}
std::vector<std::string> analyze(const GlobalIndex &index) {
  std::vector<Diagnostic> out;
  analyzeIsaLeaks(index, kIsaLeakDefaultFacets, out);
  return messages(out);
}
} // namespace

TEST_CASE("isa-leak names one witness per group and counts the rest",
          "[CheckIsaLeak]") {
  GlobalIndex index;
  IsaLeakFacts &facts = index.mutableIsaLeakFacts();
  const uint32_t avx = facts.addProfile(x86Profile({"avx", "sse2"}));
  const uint32_t base = facts.addProfile(x86Profile({"sse2"}));
  facts.addTu("/p/z.cpp", base, {loopFunction()});
  facts.addTu("/p/a.cpp", avx, {loopFunction()});
  facts.addTu("/p/c.cpp", base, {loopFunction()});
  facts.addTu("/p/b.cpp", base, {loopFunction()});
  auto result = analyze(index);
  REQUIRE(result.size() == 1);
  CHECK(result[0].find("[1] /p/a.cpp (target features +avx)") !=
        std::string::npos);
  CHECK(result[0].find("[2] /p/b.cpp and 2 more TUs (no extra target "
                       "features)") != std::string::npos);
}

TEST_CASE("isa-leak remaps ids when stores intern profiles in another order",
          "[CheckIsaLeak]") {
  GlobalIndex source;
  {
    IsaLeakFacts &facts = source.mutableIsaLeakFacts();
    const uint32_t avx = facts.addProfile(x86Profile({"avx", "sse2"}));
    const uint32_t base = facts.addProfile(x86Profile({"sse2"}));
    facts.addTu("/p/a.cpp", avx, {loopFunction()});
    facts.addTu("/p/b.cpp", base, {loopFunction()});
  }
  const auto expected = analyze(source);
  REQUIRE(expected.size() == 1);

  // Pre-seeded with the profiles reversed and an unrelated TU first.
  auto seeded = [] {
    auto index = std::make_unique<GlobalIndex>();
    IsaLeakFacts &facts = index->mutableIsaLeakFacts();
    facts.addProfile(x86Profile({"sse2"}));
    const uint32_t other = facts.addProfile(x86Profile({"avx2", "sse2"}));
    EmittedFunction f = loopFunction();
    f.linkageName = "_Z5otherv";
    f.qualifiedName = "other()";
    facts.addTu("/p/other.cpp", other, {f});
    return index;
  };
  auto absorbed = seeded();
  absorbed->mutableIsaLeakFacts().absorb(source.isaLeakFacts());
  CHECK(analyze(*absorbed) == expected);

  IsaLeakPayload payload;
  source.isaLeakFacts().capture(payload);
  CHECK(payload.tus.size() == 2);
  CHECK(payload.functions.size() == 1);
  CHECK(payload.sightings.size() == 2);
  auto applied = seeded();
  applied->mutableIsaLeakFacts().apply(payload);
  CHECK(analyze(*applied) == expected);

  // Out-of-range ids (a damaged payload) drop the sighting, nothing else.
  payload.sightings.push_back({7, 0, 0});
  payload.sightings.push_back({0, 9, 0});
  payload.sightings.push_back({0, 0, 5});
  auto damaged = seeded();
  damaged->mutableIsaLeakFacts().apply(payload);
  CHECK(analyze(*damaged) == expected);
  CHECK(damaged->isaLeakFacts().sightingCount() == 3);
}

TEST_CASE("isa-leak rejects a shard whose sightings name no table entry",
          "[CheckIsaLeak]") {
  GlobalIndex index;
  IsaLeakFacts &facts = index.mutableIsaLeakFacts();
  facts.addTu("/p/a.cpp", facts.addProfile(x86Profile({"sse2"})),
              {loopFunction()});
  auto payload = CheckIndexPayload::capture(index);
  REQUIRE(payload.isaLeak.sightings.size() == 1);
  payload.isaLeak.sightings[0].tu = 4;
  llvm::SmallString<128> dir;
  REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("isa_leak_shard", dir));
  const std::string shard = std::string(dir) + "/worker.bin";
  REQUIRE(writeCheckIndexShard(shard, {{"a.cpp", payload}},
                               {{TuStatus::Indexed, ""}}));
  bool called = false;
  CHECK_FALSE(readCheckIndexShard(
      shard, [&](const auto &, const auto &) { called = true; }));
  CHECK_FALSE(called);
  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("isa-leak turns the checkpoint journal off", "[CheckIsaLeak]") {
  IsaFixture fixture(mixedHeader, mixedUses);
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  auto opts = isaOptions();
  const auto expected = messages(runAnalysis(*db, fixture.files(), opts));
  REQUIRE(expected.size() == 2);
  opts.checkpointPath = fixture.dir + "/journal.bin";
  CHECK(messages(runAnalysis(*db, fixture.files(), opts)) == expected);
  CHECK_FALSE(llvm::sys::fs::exists(opts.checkpointPath));
}

TEST_CASE("isa-leak skips branches Clang folds away", "[CheckIsaLeak]") {
  // Every call to work() sits in a branch whose condition folds: no TU
  // emits it, even at -O0.
  IsaFixture fixture(R"cpp(
inline float work(const float *a, unsigned long n) {
  float s = 0;
  for (unsigned long i = 0; i < n; ++i) s += a[i];
  return s;
}
constexpr bool kSimd = false;
inline float gate(const float *a) {
  if constexpr (kSimd) return work(a, 1);
  if (kSimd) return work(a, 2);
  if (sizeof(float) == 8) return work(a, 3);
  float r = kSimd ? work(a, 4) : 0.0f;
  if (kSimd && work(a, 5) > 0) r = 1;
  if (!kSimd || work(a, 6) > 0) r += 1;
  return r;
}
)cpp",
                     "  float v[2] = {};\n  (void)gate(v);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  (void)runAnalysis(*db, fixture.files(), isaOptions(), &index);
  std::set<std::string> names;
  for (const auto &r : index.isaLeakFacts().records())
    names.insert(r.function.qualifiedName);
  CHECK(names.count("gate(const float *)"));
  CHECK_FALSE(names.count("work(const float *, unsigned long)"));
}

TEST_CASE("isa-leak leaves fast-math alone for sign operations and integer "
          "vectors",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
typedef int v4si __attribute__((vector_size(16)));
inline double magnitude(double x) { return __builtin_fabs(x); }
inline double sign(double x, double y) { return __builtin_copysign(x, y); }
inline v4si add4(v4si a, v4si b) { return a + b; }
)cpp",
                     "  (void)magnitude(-1.0);\n"
                     "  (void)sign(1.0, -1.0);\n  v4si x = {1, 2, 3, 4};\n"
                     "  (void)add4(x, x);\n");
  fixture.database({"-ffast-math"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  CHECK(index.isaLeakFacts().functionCount() == 3);
}

TEST_CASE("isa-leak emits a key-less vtable where the constructor is",
          "[CheckIsaLeak]") {
  // W's constructor is defined elsewhere: that TU stores the vtable
  // pointer and emits the vtable, not the TUs that construct a W.
  IsaFixture fixture(R"cpp(
struct W {
  W();
  virtual float draw(const float *a, unsigned long n) const {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
)cpp",
                     "  W w;\n  (void)&w;\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  CHECK(index.isaLeakFacts().functionCount() == 0);
}

TEST_CASE("isa-leak follows virtual operators and delete through the vtable",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
struct V {
  virtual ~V();
  virtual float operator()(const float *a, unsigned long n) const {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
struct Leaf final : V {
  ~Leaf() override { float s = 0; for (int i = 0; i < 3; ++i) s += 1.5f; sink = s; }
  float sink = 0;
};
)cpp",
                     "  extern V *vp;\n  float v[2] = {};\n"
                     "  (void)(*vp)(v, 2);\n  delete vp;\n"
                     "  extern Leaf *leaf;\n  delete leaf;\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index));
  // operator() and ~V are called through the vtable. Leaf is final, so
  // deleting it calls ~Leaf directly, and ~Leaf does not reset the vtable
  // pointer, so Leaf's vtable (and the inherited operator()) is not
  // emitted by it either.
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl == "Leaf::~Leaf()");
}

TEST_CASE("isa-leak emits variable template instantiations with dynamic "
          "initializers",
          "[CheckIsaLeak]") {
  // scaled<int> is instantiated by a reference from an inline function
  // nothing calls; its initializer still runs in every TU.
  IsaFixture fixture(R"cpp(
inline float grow(unsigned long n) {
  float s = 1;
  for (unsigned long i = 0; i < n; ++i) s *= 1.5f;
  return s;
}
template <typename T> inline float scaled = grow(sizeof(T));
inline float unusedRef() { return scaled<int>; }
)cpp");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl == "grow(unsigned long)");
}

TEST_CASE("isa-leak compares bit-manipulation features for bit builtins",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
inline int parity(unsigned x) { return __builtin_parity(x); }
inline int first(unsigned x) { return __builtin_ffs(static_cast<int>(x)); }
inline unsigned plain(unsigned x) { return x ^ 1u; }
)cpp",
                     "  (void)parity(3);\n  (void)first(8);\n  (void)plain(1);\n");
  fixture.database({"-mpopcnt", "-mbmi"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 2);
  for (const auto &d : result) {
    CHECK(d.resolvedDecl.find("plain") == std::string::npos);
    CHECK(mentions(d, "bit-manipulation builtins"));
  }
}

TEST_CASE("isa-leak orders groups one file witnesses by content",
          "[CheckIsaLeak]") {
  // a.cpp built twice (a SIMD library and a plain one) witnesses both
  // groups: the message must not depend on which build came first.
  auto run = [](bool avxFirst) {
    GlobalIndex index;
    IsaLeakFacts &facts = index.mutableIsaLeakFacts();
    const CodegenProfile avx = x86Profile({"avx", "sse2"});
    const CodegenProfile base = x86Profile({"sse2"});
    if (avxFirst) {
      facts.addTu("/p/a.cpp", facts.addProfile(avx), {loopFunction()});
      facts.addTu("/p/a.cpp", facts.addProfile(base), {loopFunction()});
    } else {
      facts.addTu("/p/a.cpp", facts.addProfile(base), {loopFunction()});
      facts.addTu("/p/a.cpp", facts.addProfile(avx), {loopFunction()});
    }
    return analyze(index);
  };
  const auto first = run(true);
  REQUIRE(first.size() == 1);
  CHECK(run(false) == first);
}

namespace {
std::set<std::string> emittedNames(const GlobalIndex &index) {
  std::set<std::string> names;
  for (const auto &r : index.isaLeakFacts().records())
    names.insert(r.function.qualifiedName);
  return names;
}
} // namespace

TEST_CASE("isa-leak keeps dead branches CodeGen still emits",
          "[CheckIsaLeak]") {
  // A label or a case label inside a folded-away branch makes it
  // reachable, and a class-type ?: emits both arms whatever its condition.
  IsaFixture fixture(R"cpp(
inline float work(const float *a, unsigned long n) {
  float s = 0;
  for (unsigned long i = 0; i < n; ++i) s += a[i];
  return s;
}
inline float more(const float *a, unsigned long n) {
  float s = 1;
  for (unsigned long i = 0; i < n; ++i) s *= a[i];
  return s;
}
struct Str { Str(); Str(const Str &); ~Str(); };
inline Str slowFormat(const float *a) { Str s; (void)more(a, 3); return s; }
constexpr bool kDebug = false;
inline float jump(const float *a, int n) {
  float r = 0;
  switch (n) { case 0: if (0) { case 1: r = work(a, 1); } break; }
  if (n == 7) goto later;
  if (0) { later: r += 1; }
  Str s = kDebug ? slowFormat(a) : Str();
  return r;
}
)cpp",
                     "  float v[2] = {};\n  (void)jump(v, 1);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  (void)runAnalysis(*db, fixture.files(), isaOptions(), &index);
  const auto names = emittedNames(index);
  CHECK(names.count("jump(const float *, int)"));
  CHECK(names.count("work(const float *, unsigned long)"));
  CHECK(names.count("slowFormat(const float *)"));
  CHECK(names.count("more(const float *, unsigned long)"));
}

TEST_CASE("isa-leak reports float vector builtins and classification under "
          "fast-math",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
typedef float v4sf __attribute__((vector_size(16)));
typedef int v4si __attribute__((vector_size(16)));
inline v4sf root(v4sf a) { return __builtin_elementwise_sqrt(a); }
inline v4si narrow(v4sf a) { return __builtin_convertvector(a, v4si); }
inline int above(float x) { return __builtin_isgreater(x, 1.0f); }
)cpp",
                     "  v4sf v = {1, 2, 3, 4};\n  (void)root(v);\n"
                     "  (void)narrow(v);\n  (void)above(2.0f);\n");
  fixture.database({"-ffast-math"}, {});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  std::set<std::string> reported;
  for (const auto &d : result) {
    reported.insert(d.resolvedDecl);
    CHECK(mentions(d, "(fp)"));
  }
  CHECK(reported.count("root(v4sf)"));
  CHECK(reported.count("narrow(v4sf)"));
  CHECK(reported.count("above(float)"));
}

TEST_CASE("isa-leak emits a key-less vtable where its destructor resets it",
          "[CheckIsaLeak]") {
  // Neither class's constructor is defined here. Loud's destructor has a
  // body, so emitting it stores the vptr and emits the vtable; Quiet's
  // destructor and its field's have trivial bodies, so CodeGen skips the
  // store and Quiet's vtable stays unemitted.
  IsaFixture fixture(R"cpp(
struct Inner { ~Inner() {} };
void note();
struct Loud {
  Loud();
  ~Loud() { note(); }
  virtual float sum(const float *a, unsigned long n) const {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
struct Quiet {
  Quiet();
  ~Quiet() {}
  Inner inner;
  virtual float total(const float *a, unsigned long n) const {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
inline void kill(Loud *l, Quiet *q) { delete l; delete q; }
)cpp",
                     "  kill(nullptr, nullptr);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  (void)runAnalysis(*db, fixture.files(), isaOptions(), &index);
  const auto names = emittedNames(index);
  CHECK(names.count("Loud::~Loud()"));
  CHECK(names.count("Quiet::~Quiet()"));
  CHECK(names.count("Loud::sum(const float *, unsigned long) const"));
  CHECK_FALSE(names.count("Quiet::total(const float *, unsigned long) const"));
}

TEST_CASE("isa-leak emits a static local's destructor and final-class "
          "virtual calls",
          "[CheckIsaLeak]") {
  IsaFixture fixture(R"cpp(
inline float flush(const float *a, unsigned long n) {
  float s = 0;
  for (unsigned long i = 0; i < n; ++i) s += a[i];
  return s;
}
struct Logger { float buf[4]; ~Logger() { (void)flush(buf, 4); } };
inline Logger &logger() { static Logger l; return l; }
struct Base {
  virtual void anchor();
  virtual float sum(const float *a, unsigned long n) const;
};
struct Fin final : Base {
  void anchor() override;
  float sum(const float *a, unsigned long n) const override {
    float s = 0;
    for (unsigned long i = 0; i < n; ++i) s += a[i];
    return s;
  }
};
inline float call(const Fin *f, const float *a) { return f->sum(a, 2); }
)cpp",
                     "  (void)logger();\n  float v[2] = {};\n"
                     "  (void)call(nullptr, v);\n");
  fixture.database({"-mavx2"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  (void)runAnalysis(*db, fixture.files(), isaOptions(), &index);
  const auto names = emittedNames(index);
  CHECK(names.count("Logger::~Logger()"));
  CHECK(names.count("flush(const float *, unsigned long)"));
  CHECK(names.count("Fin::sum(const float *, unsigned long) const"));
}

TEST_CASE("isa-leak compares move-only bodies on the vector class off x86-64",
          "[CheckIsaLeak]") {
  // On i386 an FP value moves through x87 or SSE registers, so SSE
  // matters even to a body that only loads and returns a float. The CPU
  // is pinned: the default i686 CPU differs between Clang releases.
  IsaFixture fixture(R"cpp(
inline float pass(const float *p) { return *p; }
)cpp",
                     "  float v = 1;\n  (void)pass(&v);\n");
  fixture.database({"--target=i686-unknown-linux-gnu", "-march=pentium4"},
                   {"--target=i686-unknown-linux-gnu", "-march=pentium4",
                    "-mno-sse"});
  auto db = fixture.db();
  auto result = isaOnly(runAnalysis(*db, fixture.files(), isaOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].resolvedDecl == "pass(const float *)");
  CHECK(mentions(result[0], "+sse"));
  CHECK(mentions(result[0], "floating-point values"));
}

TEST_CASE("isa-leak rejects a shard whose profile features are unsorted",
          "[CheckIsaLeak]") {
  GlobalIndex index;
  IsaLeakFacts &facts = index.mutableIsaLeakFacts();
  facts.addTu("/p/a.cpp", facts.addProfile(x86Profile({"avx", "sse2"})),
              {loopFunction()});
  auto payload = CheckIndexPayload::capture(index);
  REQUIRE(payload.isaLeak.profiles.size() == 1);
  payload.isaLeak.profiles[0].features = {"sse2", "avx"};
  llvm::SmallString<128> dir;
  REQUIRE_FALSE(llvm::sys::fs::createUniqueDirectory("isa_leak_shard", dir));
  const std::string shard = std::string(dir) + "/worker.bin";
  REQUIRE(writeCheckIndexShard(shard, {{"a.cpp", payload}},
                               {{TuStatus::Indexed, ""}}));
  bool called = false;
  CHECK_FALSE(readCheckIndexShard(
      shard, [&](const auto &, const auto &) { called = true; }));
  CHECK_FALSE(called);
  llvm::sys::fs::remove_directories(dir);
}

TEST_CASE("isa-leak leaves fast-math alone for addressof on a double",
          "[CheckIsaLeak]") {
  // __builtin_addressof (like std::move, std::forward, std::addressof) is
  // folded into a cast: no floating-point code, whatever its operand.
  IsaFixture fixture(R"cpp(
inline const double *where(const double &d) { return __builtin_addressof(d); }
)cpp",
                     "  double d = 1;\n  (void)where(d);\n");
  fixture.database({"-ffast-math"}, {});
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(isaOnly(runAnalysis(*db, fixture.files(), isaOptions(), &index))
            .empty());
  CHECK(emittedNames(index).count("where(const double &)"));
}
