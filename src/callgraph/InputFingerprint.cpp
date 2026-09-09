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

#include "vycor/callgraph/InputFingerprint.h"

#include "vycor/Version.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/compat/ToolAdjusters.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/SHA1.h"

#include <cstdint>

namespace vycor {

namespace {

/// Bumped when the set or framing of hashed fields changes, so an index
/// fingerprinted by an older scheme never compares equal by accident.
constexpr uint32_t kFingerprintSchema = 1;

/// Length-prefixed framing: `["a b"]` and `["a", "b"]` hash differently,
/// and so do the same strings in another order.
class Hasher {
public:
  void str(llvm::StringRef s) {
    u64(s.size());
    sha_.update(s);
  }
  void u64(uint64_t v) {
    uint8_t buf[8];
    for (int i = 0; i < 8; ++i)
      buf[i] = static_cast<uint8_t>(v >> (8 * i));
    sha_.update(llvm::ArrayRef<uint8_t>(buf, 8));
  }
  void list(const std::vector<std::string> &items) {
    u64(items.size());
    for (const auto &s : items)
      str(s);
  }
  std::string hex() {
    auto digest = sha_.final();
    return llvm::toHex(llvm::ArrayRef<uint8_t>(digest.data(), digest.size()),
                       /*LowerCase=*/true);
  }

private:
  llvm::SHA1 sha_;
};

} // namespace

BakeEnvironment BakeEnvironment::current(const std::string &sysroot,
                                         const std::string &pchDir) {
  BakeEnvironment env;
  env.sysroot = sysroot;
  env.extraArgs = globalExtraArgs();
  env.pchDir = pchDir;
  return env;
}

std::string analyzerIdentity() {
  return std::string("vycor-cpp ") + VYCOR_VERSION_STRING +
         ", index format " + std::to_string(SnapshotIO::kFormatVersion);
}

std::string toolchainIdentity() {
  return std::string("LLVM ") + LLVM_VERSION_STRING;
}

std::string environmentFingerprint(const BakeEnvironment &env) {
  Hasher h;
  h.u64(kFingerprintSchema);
  h.str(analyzerIdentity());
  h.str(toolchainIdentity());
#ifdef VYCOR_CLANG_RESOURCE_DIR
  h.str(VYCOR_CLANG_RESOURCE_DIR);
#else
  h.str("");
#endif
#ifdef VYCOR_DEFAULT_SYSROOT
  h.str(VYCOR_DEFAULT_SYSROOT);
#else
  h.str("");
#endif
  // The GCC installation the adjuster would point Clang at: a runtime
  // discovery of /usr/lib/gcc, so a distro upgrade that changes the
  // pick changes every TU's fingerprint (and the headers it finds).
  h.str(detectUsableGccInstallDir());
  h.str(env.sysroot);
  h.list(env.extraArgs);
  h.str(env.pchDir);
  return h.hex();
}

std::vector<std::string>
fingerprintTUs(const clang::tooling::CompilationDatabase &compDb,
               const std::vector<std::string> &files,
               const std::string &envFingerprint) {
  std::vector<std::string> out;
  out.reserve(files.size());
  for (const auto &file : files) {
    Hasher h;
    h.str(envFingerprint);
    auto cmds = compDb.getCompileCommands(file);
    h.u64(cmds.size());
    for (const auto &cmd : cmds) {
      h.str(cmd.Directory);
      h.str(cmd.Filename);
      h.str(cmd.Output);
      h.list(cmd.CommandLine);
    }
    out.push_back(h.hex());
  }
  return out;
}

} // namespace vycor
