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

#pragma once

#include "clang/Tooling/CompilationDatabase.h"

#include <string>
#include <vector>

namespace vycor {

// ============================================================================
// Effective-input fingerprints — what, besides the source files, decides
// the facts a TU's parse produces. A warm start compares the fingerprint
// recorded for each TU with the one computed now and re-indexes on any
// difference, so a compile-command edit (a new -D, a moved -I, a different
// working directory) refreshes the TU the way a source edit does
// (docs/index-provenance.md).
//
// Two layers:
//   - the bake environment: the analyzer and toolchain identity plus the
//     ambient inputs every TU shares (extra args, sysroot, PCH cache, the
//     GCC installation the tool would pick). One fingerprint per bake;
//     a change makes every TU dirty, which the warm start turns into a
//     full rebuild.
//   - per TU: the environment fingerprint plus every compile command the
//     database holds for the file, in database order, each with its
//     working directory, file name, output, and argument list verbatim.
//     A file with several commands is one TU with several inputs; the
//     variants are hashed in sequence, never merged.
//
// Fingerprints are hex SHA-1 digests over length-prefixed fields, so the
// same inputs give the same string on every host and argument order
// matters. They are opaque: compare for equality, never parse.
// ============================================================================

struct BakeEnvironment {
  std::string sysroot;                // --sysroot (empty = compiled default)
  std::vector<std::string> extraArgs; // --extra-arg / VYCOR_EXTRA_ARGS
  std::string pchDir;                 // --pch-dir (empty = off)

  /// The environment of the current process: `extraArgs` from
  /// globalExtraArgs(), the rest from the given flags.
  static BakeEnvironment current(const std::string &sysroot,
                                 const std::string &pchDir);
};

/// Human-readable analyzer identity ("vycor-cpp 0.2.0, index format 10")
/// and toolchain identity ("LLVM 20.1.2"), recorded in the index
/// provenance alongside the fingerprint that folds them in.
std::string analyzerIdentity();
std::string toolchainIdentity();

/// Fingerprint of the bake environment: analyzer and toolchain identity,
/// the compiled-in resource directory and default sysroot, the detected
/// GCC installation, and every BakeEnvironment field.
std::string environmentFingerprint(const BakeEnvironment &env);

/// Per-TU fingerprints, parallel to `files`: `envFingerprint` plus the
/// file's compile commands. A file the database has no command for hashes
/// to the environment alone (its parse would be skipped either way).
std::vector<std::string>
fingerprintTUs(const clang::tooling::CompilationDatabase &compDb,
               const std::vector<std::string> &files,
               const std::string &envFingerprint);

} // namespace vycor
