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

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

#include <catch2/catch_test_macros.hpp>
#include <clang/Tooling/CompilationDatabase.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

using namespace vycor;
namespace {
struct AbiFixture {
  std::string dir;
  AbiFixture(const std::string &header) {
    llvm::SmallString<128> path;
    REQUIRE_FALSE(
        llvm::sys::fs::createUniqueDirectory("abi_layout_test", path));
    llvm::SmallString<256> absolute;
    REQUIRE_FALSE(llvm::sys::fs::real_path(path, absolute));
    dir = std::string(absolute);
    write("record.h", header);
    write("a.cpp", "#include \"record.h\"\n");
    write("b.cpp", "#include \"record.h\"\n");
    database({"-DPACKED"}, {});
  }
  ~AbiFixture() { llvm::sys::fs::remove_directories(dir); }
  void write(const std::string &name, const std::string &body) const {
    std::ofstream file(dir + "/" + name);
    REQUIRE(file.good());
    file << body;
  }
  std::vector<std::string> files() const {
    return {dir + "/a.cpp", dir + "/b.cpp"};
  }
  void database(const std::vector<std::string> &left,
                const std::vector<std::string> &right,
                bool sameTu = false) const {
    llvm::json::Array entries;
    auto paths = files();
    if (sameTu)
      paths[1] = paths[0];
    for (unsigned i = 0; i < paths.size(); ++i) {
      llvm::json::Array args{"clang++", "-std=c++17", "-I" + dir};
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
AnalysisOptions abiOptions() {
  AnalysisOptions opts;
  opts.threadCount = 1;
  opts.enableAbiLayoutDiag = true;
  return opts;
}
std::vector<Diagnostic> abiOnly(const std::vector<Diagnostic> &all) {
  std::vector<Diagnostic> out;
  for (const auto &d : all)
    if (d.kind == Diagnostic::AbiLayout_Divergent)
      out.push_back(d);
  return out;
}
std::vector<std::string> messages(const std::vector<Diagnostic> &all) {
  std::vector<std::string> out;
  for (const auto &d : abiOnly(all))
    out.push_back(d.callLocation + " " + d.message);
  return out;
}
const char *packedHeader = R"cpp(
#ifdef PACKED
#pragma pack(push, 1)
#endif
struct Packet { char tag; long payload; };
struct alignas(16) OffsetOnly { char tag; int value; };
#ifdef PACKED
#pragma pack(pop)
#endif
)cpp";
} // namespace

TEST_CASE("ABI layout detects packing and offsets even with equal total size",
          "[CheckAbi]") {
  AbiFixture fixture(packedHeader);
  auto db = fixture.db();
  GlobalIndex index;
  auto opts = abiOptions();
  auto result = abiOnly(runAnalysis(*db, fixture.files(), opts, &index));
  REQUIRE(result.size() == 2);
  CHECK(index.abiLayoutCount() == 4);
  CHECK(std::any_of(result.begin(), result.end(), [](const auto &d) {
    return d.message.find("'OffsetOnly'") != std::string::npos &&
           d.message.find("offset(bits)") != std::string::npos;
  }));
  for (const auto &d : result) {
    CHECK(d.checkName == "abi-layout-divergence");
    CHECK(d.message.find("a.cpp") != std::string::npos);
    CHECK(d.message.find("b.cpp") != std::string::npos);
  }
  auto reversed = fixture.files();
  std::reverse(reversed.begin(), reversed.end());
  opts.threadCount = 2;
  CHECK(messages(runAnalysis(*db, reversed, opts)) == messages(result));
}

TEST_CASE("ABI check ignores identical layouts, namespaces and function-local "
          "classes",
          "[CheckAbi]") {
  AbiFixture fixture(R"cpp(
struct Stable { char tag; long payload; };
struct Forward;
namespace one { struct Same { int value; }; }
namespace two { struct Same { double value; }; }
namespace { struct Private { int value; }; }
inline void a() { struct Row { int value; }; Row r{}; (void)r; }
inline void b() { struct Row { double value; }; Row r{}; (void)r; }
struct Base { virtual ~Base() = default; int x; };
struct Derived : virtual Base { int y; };
union Choice { int x; double y; };
)cpp");
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(
      abiOnly(runAnalysis(*db, fixture.files(), abiOptions(), &index)).empty());
  index.forEachAbiLayout([](const AbiLayoutEntry &entry) {
    CHECK(entry.qualifiedName != "Row");
    CHECK(entry.qualifiedName != "Private");
    CHECK(entry.qualifiedName != "Forward");
  });
}

TEST_CASE("ABI check captures bitfield widths and inherited base offsets",
          "[CheckAbi]") {
  AbiFixture fixture(R"cpp(
#ifndef WIDTH
#define WIDTH 3
#endif
struct Bits { unsigned first: WIDTH; unsigned second: 5; };
struct Parent { char tag; int payload; };
struct Child : Parent { char tail; };
)cpp");
  fixture.database({"-DWIDTH=4", "-fpack-struct=1"}, {});
  auto db = fixture.db();
  auto result = abiOnly(runAnalysis(*db, fixture.files(), abiOptions()));
  CHECK(result.size() == 3);
  CHECK(std::any_of(result.begin(), result.end(), [](const auto &d) {
    return d.message.find("'Child'") != std::string::npos;
  }));
  // Keep total size/alignment equal to exercise bit widths independently.
  fixture.database({"-DWIDTH=4"}, {});
  db = fixture.db();
  result = abiOnly(runAnalysis(*db, fixture.files(), abiOptions()));
  REQUIRE(result.size() == 1);
  CHECK(result[0].message.find("bit-width") != std::string::npos);
}

TEST_CASE("ABI layouts partition different target architectures",
          "[CheckAbi]") {
  AbiFixture fixture("struct Pointer { void *p; long value; };\n");
  fixture.database({"--target=i386-unknown-linux-gnu"},
                   {"--target=x86_64-unknown-linux-gnu"});
  auto db = fixture.db();
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), abiOptions())).empty());
}

TEST_CASE("ABI facts survive absorb, worker shards and index handoffs",
          "[CheckAbi]") {
  AbiFixture fixture(packedHeader);
  auto db = fixture.db();
  GlobalIndex original;
  auto expected =
      messages(runAnalysis(*db, fixture.files(), abiOptions(), &original));
  GlobalIndex merged;
  merged.absorb(original);
  merged.absorb(original);
  CHECK(merged.abiLayoutCount() == original.abiLayoutCount());
  const auto handoff = fixture.dir + "/handoff.bin";
  REQUIRE(writeGlobalIndexFile(handoff, merged));
  GlobalIndex loaded;
  REQUIRE(readGlobalIndexFile(handoff, loaded));
  std::vector<Diagnostic> result;
  analyzeAbiLayoutDivergence(loaded, result);
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
  analyzeAbiLayoutDivergence(replayed, result);
  CHECK(messages(result) == expected);
}

TEST_CASE("ABI flag is opt-in and changes the checkpoint fingerprint",
          "[CheckAbi]") {
  auto opts = abiOptions();
  const auto enabled = checkOptionsFingerprint(opts);
  opts.enableAbiLayoutDiag = false;
  CHECK(checkOptionsFingerprint(opts) != enabled);
  CHECK_FALSE(defaultCheckSet().count("abi-layout-divergence"));
  std::set<std::string> checks;
  std::string error;
  REQUIRE(resolveCheckSpec({"-all", "abi-layout-divergence"}, checks, error));
  CHECK(checks == std::set<std::string>{"abi-layout-divergence"});
  AbiFixture fixture(packedHeader);
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), opts, &index)).empty());
  CHECK(index.abiLayoutCount() == 0);
}

TEST_CASE(
    "ABI checking uses fresh layouts after flag-only or header-only edits",
    "[CheckAbi]") {
  AbiFixture fixture(packedHeader);
  auto opts = abiOptions();
  opts.checkpointPath = fixture.dir + "/checkpoint.bin";
  auto db = fixture.db();
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), opts)).size() == 2);
  fixture.database({}, {});
  db = fixture.db();
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), opts)).empty());
  fixture.database({"-DALT"}, {});
  fixture.write("record.h", "struct Packet { int value; };\n");
  db = fixture.db();
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), opts)).empty());
  fixture.write("record.h",
                "#ifdef ALT\nstruct Packet { long value; };\n#else\nstruct "
                "Packet { int value; };\n#endif\n");
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), opts)).size() == 1);
}

TEST_CASE(
    "ABI check treats zero-length and flexible array tails as zero storage",
    "[CheckAbi]") {
  AbiFixture fixture(R"cpp(
#ifdef PACKED
struct Tail { int n; char data[]; };
#else
struct Tail { int n; char data[0]; };
#endif
)cpp");
  auto db = fixture.db();
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), abiOptions())).empty());
}

TEST_CASE("ABI check requires two distinct TUs, not two commands for one TU",
          "[CheckAbi]") {
  AbiFixture fixture(packedHeader);
  fixture.database({"-DPACKED"}, {}, true);
  auto db = fixture.db();
  GlobalIndex index;
  CHECK(abiOnly(runAnalysis(*db, {fixture.files()[0]}, abiOptions(), &index))
            .empty());
  CHECK(index.abiLayoutCount() == 4); // both layouts were actually collected
}

TEST_CASE("ABI layout ignores redundant using declarations", "[CheckAbi]") {
  AbiFixture fixture(R"cpp(
namespace Math {
struct Vector3 { float x, y, z; };
#ifdef PACKED
using Math::Vector3;
#endif
struct Holder { Vector3 v; };
inline float x() { Vector3 v{}; return v.x; }
}
)cpp");
  auto db = fixture.db();
  CHECK(abiOnly(runAnalysis(*db, fixture.files(), abiOptions())).empty());
}

TEST_CASE("ABI payload rejects previous-version handoffs", "[CheckAbi]") {
  AbiFixture fixture("struct Value { int v; };\n");
  GlobalIndex index;
  const auto path = fixture.dir + "/old.bin";
  REQUIRE(writeGlobalIndexFile(path, index));
  std::ifstream input(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(input)), {});
  REQUIRE(bytes.size() >= 8);
  bytes[4] = 11;
  bytes[5] = bytes[6] = bytes[7] = 0;
  fixture.write("old.bin", bytes);
  CHECK_FALSE(readGlobalIndexFile(path, index));
}
