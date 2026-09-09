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

// test_input_fingerprint.cpp — the effective-input fingerprint contract
// (docs/index-provenance.md): deterministic, sensitive to every field that
// changes a TU's parse, insensitive to nothing that does.

#include "vycor/callgraph/InputFingerprint.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/compat/ToolAdjusters.h"

#include "clang/Tooling/CompilationDatabase.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

using namespace vycor;

namespace {

/// A database whose commands are exactly what the test hands it.
class FixedDb : public clang::tooling::CompilationDatabase {
public:
  std::vector<clang::tooling::CompileCommand> cmds;

  std::vector<clang::tooling::CompileCommand>
  getCompileCommands(llvm::StringRef file) const override {
    std::vector<clang::tooling::CompileCommand> out;
    for (const auto &c : cmds)
      if (c.Filename == file)
        out.push_back(c);
    return out;
  }
  std::vector<std::string> getAllFiles() const override {
    std::vector<std::string> out;
    for (const auto &c : cmds)
      out.push_back(c.Filename);
    return out;
  }
  std::vector<clang::tooling::CompileCommand>
  getAllCompileCommands() const override {
    return cmds;
  }
};

clang::tooling::CompileCommand cmd(const std::string &dir,
                                   const std::string &file,
                                   std::vector<std::string> args,
                                   const std::string &output = "") {
  return clang::tooling::CompileCommand(dir, file, std::move(args), output);
}

bool isHexDigest(const std::string &s) {
  if (s.size() != 40)
    return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  return true;
}

} // namespace

TEST_CASE("the environment fingerprint is deterministic and covers every "
          "ambient input",
          "[fingerprint]") {
  BakeEnvironment base;
  base.sysroot = "";
  base.extraArgs = {"-DX=1", "--gcc-install-dir=/usr/lib/gcc/x/14"};
  base.pchDir = "";
  const std::string fp = environmentFingerprint(base);
  CHECK(isHexDigest(fp));
  CHECK(environmentFingerprint(base) == fp);

  SECTION("identity strings name the analyzer, format, and toolchain") {
    CHECK(analyzerIdentity().find("vycor-cpp ") == 0);
    CHECK(analyzerIdentity().find("index format " + std::to_string(
                                      SnapshotIO::kFormatVersion)) !=
          std::string::npos);
    CHECK(toolchainIdentity().find("LLVM ") == 0);
  }

  SECTION("sysroot") {
    auto e = base;
    e.sysroot = "/opt/sdk";
    CHECK(environmentFingerprint(e) != fp);
  }
  SECTION("extra args: content, order, and splitting") {
    auto e = base;
    e.extraArgs = {"--gcc-install-dir=/usr/lib/gcc/x/14", "-DX=1"};
    CHECK(environmentFingerprint(e) != fp);
    e.extraArgs = {"-DX=1 --gcc-install-dir=/usr/lib/gcc/x/14"};
    CHECK(environmentFingerprint(e) != fp);
    e.extraArgs = {"-DX=1", "--gcc-install-dir=/usr/lib/gcc/x/14", ""};
    CHECK(environmentFingerprint(e) != fp);
  }
  SECTION("PCH directory") {
    auto e = base;
    e.pchDir = "/tmp/pch";
    CHECK(environmentFingerprint(e) != fp);
  }
  SECTION("BakeEnvironment::current reads the process's extra args") {
    auto e = BakeEnvironment::current("/sr", "/pch");
    CHECK(e.sysroot == "/sr");
    CHECK(e.pchDir == "/pch");
    CHECK(e.extraArgs == globalExtraArgs());
  }
}

TEST_CASE("TU fingerprints follow the compile commands", "[fingerprint]") {
  const std::string env = environmentFingerprint(BakeEnvironment{});
  FixedDb db;
  db.cmds = {cmd("/w", "/w/a.cpp", {"clang++", "-std=c++17", "-c", "a.cpp"}),
             cmd("/w", "/w/b.cpp", {"clang++", "-std=c++17", "-c", "b.cpp"})};
  auto fps = fingerprintTUs(db, {"/w/a.cpp", "/w/b.cpp"}, env);
  REQUIRE(fps.size() == 2);
  CHECK(isHexDigest(fps[0]));
  CHECK(fps[0] != fps[1]);
  CHECK(fingerprintTUs(db, {"/w/a.cpp"}, env)[0] == fps[0]);

  SECTION("a new define changes only that TU") {
    db.cmds[0].CommandLine.insert(db.cmds[0].CommandLine.begin() + 1,
                                  "-DNEW_TARGET");
    auto now = fingerprintTUs(db, {"/w/a.cpp", "/w/b.cpp"}, env);
    CHECK(now[0] != fps[0]);
    CHECK(now[1] == fps[1]);
  }
  SECTION("argument order matters") {
    std::swap(db.cmds[0].CommandLine[1], db.cmds[0].CommandLine[2]);
    CHECK(fingerprintTUs(db, {"/w/a.cpp"}, env)[0] != fps[0]);
  }
  SECTION("the working directory matters") {
    db.cmds[0].Directory = "/w2";
    CHECK(fingerprintTUs(db, {"/w/a.cpp"}, env)[0] != fps[0]);
  }
  SECTION("the output matters") {
    db.cmds[0].Output = "a.o";
    CHECK(fingerprintTUs(db, {"/w/a.cpp"}, env)[0] != fps[0]);
  }
  SECTION("the environment folds in") {
    BakeEnvironment other;
    other.extraArgs = {"-DOTHER"};
    CHECK(fingerprintTUs(db, {"/w/a.cpp"},
                         environmentFingerprint(other))[0] != fps[0]);
  }
  SECTION("a second command for the same file is a distinct input, not a "
          "merge") {
    db.cmds.push_back(cmd("/w", "/w/a.cpp",
                          {"clang++", "-std=c++17", "-DVARIANT", "-c",
                           "a.cpp"}));
    auto two = fingerprintTUs(db, {"/w/a.cpp"}, env)[0];
    CHECK(two != fps[0]);
    // Order of the variants is part of the input.
    std::swap(db.cmds[0], db.cmds[2]);
    CHECK(fingerprintTUs(db, {"/w/a.cpp"}, env)[0] != two);
  }
  SECTION("a file without a command hashes the environment alone, stably") {
    auto none = fingerprintTUs(db, {"/w/none.cpp"}, env);
    REQUIRE(none.size() == 1);
    CHECK(isHexDigest(none[0]));
    CHECK(none[0] != fps[0]);
    CHECK(fingerprintTUs(db, {"/w/none.cpp"}, env)[0] == none[0]);
  }
}
