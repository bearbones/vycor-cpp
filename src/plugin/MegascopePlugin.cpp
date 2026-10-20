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

// ============================================================================
// vycor-megascope Clang Frontend Plugin (libvycor_megascope_plugin.so)
//
// Runs inside a normal `clang++ -c foo.cpp -o foo.o` invocation via
// `PluginASTAction::AddAfterMainAction`, traversing the already-parsed
// ASTContext once to emit a per-TU `.vycs` index shard alongside the `.o`
// file without re-parsing headers or templates in a separate process.
//
// Driver invocation:
//   clang++ -c foo.cpp -o foo.o
//     -fplugin=/path/to/libvycor_megascope_plugin.so
//     -Xclang -plugin-arg-vycor-megascope -Xclang --output=foo.vycs
// or:
//   clang++ -c foo.cpp -o foo.o
//     -fplugin=/path/to/libvycor_megascope_plugin.so
//     -fplugin-arg-vycor-megascope---output=foo.vycs
//
// When `--output` is omitted, the plugin defaults to replacing the compiler's
// `-o <file.o>` extension with `.vycs` (e.g. `foo.o` -> `foo.vycs`).
// ============================================================================

#include "vycor/callgraph/AtomicFile.h"
#include "vycor/callgraph/ChannelIndex.h"
#include "vycor/callgraph/ControlFlowIndex.h"
#include "vycor/callgraph/InputFingerprint.h"
#include "vycor/callgraph/MegascopeBridge.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/cli/BakeConfig.h"
#include "vycor/ext/OrgConfig.h"

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace vycor {
namespace {

struct PluginOptions {
  std::string outputPath;
  std::string emitShardPath;
  std::string emitMgscPath;
  std::vector<std::string> collapsePaths;
  std::vector<std::string> entryPoints;
  std::vector<std::string> lockTypes;
  bool useBuiltinLocks = true;
  std::string channelTypesJson;
  std::string orgConfigPath;
  bool noWait = false;
  bool durable = false;
  bool verbose = false;
};

static std::string canonicalTuPath(llvm::StringRef inFile) {
  llvm::SmallString<256> abs(inFile);
  llvm::sys::fs::make_absolute(abs);
  llvm::sys::path::remove_dots(abs, /*remove_dot_dot=*/true);
  return std::string(abs.str());
}

static std::string deriveOutputPath(const clang::CompilerInstance &ci,
                                    llvm::StringRef inFile) {
  llvm::StringRef compilerOut = ci.getFrontendOpts().OutputFile;
  if (!compilerOut.empty() && compilerOut != "-") {
    llvm::SmallString<256> out(compilerOut);
    llvm::sys::path::replace_extension(out, ".vycs");
    return std::string(out.str());
  }
  if (!inFile.empty() && inFile != "-") {
    llvm::SmallString<256> out(inFile);
    llvm::sys::path::replace_extension(out, ".vycs");
    return std::string(out.str());
  }
  return "a.vycs";
}

class MegascopePluginConsumer : public clang::ASTConsumer {
public:
  MegascopePluginConsumer(std::string tuPath, std::string outputPath,
                          std::string emitShardPath, std::string emitMgscPath,
                          std::vector<std::string> collapsePaths,
                          std::vector<std::string> entryPoints,
                          LockTypeConfig lockCfg,
                          ChannelTypeConfig channelCfg, bool noWait,
                          bool durable, bool verbose)
      : tuPath_(std::move(tuPath)), outputPath_(std::move(outputPath)),
        emitShardPath_(std::move(emitShardPath)),
        emitMgscPath_(std::move(emitMgscPath)),
        collapsePaths_(std::move(collapsePaths)),
        entryPoints_(std::move(entryPoints)), lockCfg_(std::move(lockCfg)),
        channelCfg_(std::move(channelCfg)), noWait_(noWait), durable_(durable),
        verbose_(verbose),
        bakeStartNs_(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count())) {}

  void HandleTranslationUnit(clang::ASTContext &ctx) override {
    BakedIndexes baked =
        bakeParsedTU(ctx, tuPath_, collapsePaths_, lockCfg_, channelCfg_);

    SnapshotMeta meta;
    meta.collapsePaths = collapsePaths_;
    meta.lockAllowlist = lockCfg_.userAllowlist;
    meta.lockBuiltins = lockCfg_.useBuiltins;
    meta.channelTypes = channelCfg_.registeredTypes;
    meta.files = SnapshotIO::stampFiles({tuPath_});
    SnapshotIO::recordDependencies(meta, baked.deps);
    SnapshotIO::recordOutcomes(meta, baked.outcomes);
    meta.entryPoints = entryPoints_;
    meta.provenance.analyzer = analyzerIdentity();
    meta.provenance.toolchain = toolchainIdentity();
    meta.provenance.bakeStartNs = bakeStartNs_;
    SnapshotIO::markUnstableStamps(meta, bakeStartNs_);

    auto saveOneTarget = [&](const std::string &targetPath, bool forceShard,
                             bool forceMgsc) {
      if (targetPath.empty())
        return;
      llvm::SmallString<256> parentDir(targetPath);
      llvm::sys::path::remove_filename(parentDir);
      if (!parentDir.empty())
        llvm::sys::fs::create_directories(parentDir);

      std::string lockError;
      bool busy = false;
      auto writeLock = IndexWriteLock::acquire(targetPath, !noWait_, &lockError,
                                               &busy, nullptr);
      if (!writeLock && busy) {
        llvm::errs() << "vycor-megascope plugin: " << lockError << "\n";
        return;
      }

      std::string saveError;
      bool ok = false;
      if (forceShard || MegascopeBridge::isMegascopeShardJsonPath(targetPath)) {
        ok = MegascopeBridge::saveShardJson(
            targetPath, baked.graph, baked.cfIndex, meta, baked.channels,
            &saveError, durable_);
      } else if (forceMgsc ||
                 MegascopeBridge::isMegascopeSnapshotPath(targetPath)) {
        ok = MegascopeBridge::saveSnapshot(targetPath, baked.graph,
                                           baked.cfIndex, meta, baked.channels,
                                           &saveError);
      } else {
        ok = SnapshotIO::save(targetPath, baked.graph, baked.cfIndex, meta,
                              baked.channels, &saveError, durable_);
      }
      if (!ok) {
        llvm::errs() << "vycor-megascope plugin: failed to save index to "
                     << targetPath << ": " << saveError << "\n";
        return;
      }
      if (verbose_) {
        llvm::errs() << "vycor-megascope plugin: saved " << targetPath << " ("
                     << baked.graph.nodeCount() << " nodes, "
                     << baked.graph.edgeCount() << " edges, "
                     << baked.cfIndex.size() << " call sites)\n";
      }
    };

    saveOneTarget(outputPath_, /*forceShard=*/false, /*forceMgsc=*/false);
    saveOneTarget(emitShardPath_, /*forceShard=*/true, /*forceMgsc=*/false);
    saveOneTarget(emitMgscPath_, /*forceShard=*/false, /*forceMgsc=*/true);
  }

private:
  std::string tuPath_;
  std::string outputPath_;
  std::string emitShardPath_;
  std::string emitMgscPath_;
  std::vector<std::string> collapsePaths_;
  std::vector<std::string> entryPoints_;
  LockTypeConfig lockCfg_;
  ChannelTypeConfig channelCfg_;
  bool noWait_;
  bool durable_;
  bool verbose_;
  uint64_t bakeStartNs_;
};

class MegascopePluginAction : public clang::PluginASTAction {
public:
  PluginASTAction::ActionType getActionType() override {
    return AddAfterMainAction;
  }

  bool ParseArgs(const clang::CompilerInstance &ci,
                 const std::vector<std::string> &args) override {
    clang::DiagnosticsEngine &diags = ci.getDiagnostics();
    // `args` already holds `PluginArgs["vycor-megascope"]` (populated by
    // `-Xclang -plugin-arg-vycor-megascope -Xclang <arg>`).
    std::vector<std::string> allArgs = args;
    const auto &pluginArgMap = ci.getFrontendOpts().PluginArgs;
    if (auto it = pluginArgMap.find("vycor_megascope");
        it != pluginArgMap.end()) {
      allArgs.insert(allArgs.end(), it->second.begin(), it->second.end());
    }
    // Note: Clang's driver splits `-fplugin-arg-<name>-<arg>` on the first
    // hyphen, so `-fplugin-arg-vycor-megascope---output=foo.vycs` arrives in
    // cc1 as `-plugin-arg-vycor megascope---output=foo.vycs`. Accept both
    // `vycor-megascope` and `vycor_megascope` seamlessly.
    if (auto it = pluginArgMap.find("vycor"); it != pluginArgMap.end()) {
      for (const std::string &raw : it->second) {
        llvm::StringRef ref(raw);
        if (ref.starts_with("megascope-"))
          allArgs.push_back(ref.substr(std::strlen("megascope-")).str());
        else
          allArgs.push_back(raw);
      }
    }

    for (size_t i = 0; i < allArgs.size(); ++i) {
      llvm::StringRef arg = allArgs[i];
      auto consumeValue = [&](llvm::StringRef prefix,
                              std::string &out) -> bool {
        if (arg.starts_with(prefix)) {
          out = arg.substr(prefix.size()).str();
          return true;
        }
        llvm::StringRef flag = prefix.drop_back(); // without '='
        if (arg == flag && i + 1 < allArgs.size()) {
          out = allArgs[++i];
          return true;
        }
        return false;
      };

      std::string val;
      if (consumeValue("--output=", opts_.outputPath) ||
          consumeValue("--index=", opts_.outputPath) ||
          consumeValue("-o=", opts_.outputPath)) {
        continue;
      }
      if (consumeValue("--emit-shard=", opts_.emitShardPath)) {
        continue;
      }
      if (consumeValue("--emit-mgsc=", opts_.emitMgscPath)) {
        continue;
      }
      if (consumeValue("--collapse-paths=", val)) {
        opts_.collapsePaths.push_back(std::move(val));
        continue;
      }
      if (consumeValue("--entry-point=", val) ||
          consumeValue("--entry-points=", val)) {
        opts_.entryPoints.push_back(std::move(val));
        continue;
      }
      if (consumeValue("--lock-types=", val)) {
        opts_.lockTypes.push_back(std::move(val));
        continue;
      }
      if (arg == "--no-default-locks") {
        opts_.useBuiltinLocks = false;
        continue;
      }
      if (consumeValue("--channel-types-json=", opts_.channelTypesJson)) {
        continue;
      }
      if (consumeValue("--org-config=", opts_.orgConfigPath)) {
        continue;
      }
      if (arg == "--no-wait") {
        opts_.noWait = true;
        continue;
      }
      if (arg == "--durable") {
        opts_.durable = true;
        continue;
      }
      if (arg == "--no-durable") {
        opts_.durable = false;
        continue;
      }
      if (arg == "--verbose" || arg == "-v") {
        opts_.verbose = true;
        continue;
      }
      unsigned diagId = diags.getCustomDiagID(
          clang::DiagnosticsEngine::Error,
          "vycor-megascope plugin: unknown argument '%0'");
      diags.Report(diagId) << arg;
      return false;
    }

    if (!opts_.channelTypesJson.empty() &&
        !parseChannelTypesJson(opts_.channelTypesJson, channelCfg_)) {
      unsigned diagId = diags.getCustomDiagID(
          clang::DiagnosticsEngine::Error,
          "vycor-megascope plugin: failed to load --channel-types-json '%0'");
      diags.Report(diagId) << opts_.channelTypesJson;
      return false;
    }

    if (!loadOrgConfigIfSet(opts_.orgConfigPath, orgCfg_)) {
      unsigned diagId = diags.getCustomDiagID(
          clang::DiagnosticsEngine::Error,
          "vycor-megascope plugin: failed to load --org-config '%0'");
      diags.Report(diagId) << opts_.orgConfigPath;
      return false;
    }

    return true;
  }

  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &ci,
                    llvm::StringRef inFile) override {
    std::string tuPath = canonicalTuPath(inFile);
    std::string outPath = opts_.outputPath;
    if (outPath.empty() && opts_.emitShardPath.empty() &&
        opts_.emitMgscPath.empty()) {
      outPath = deriveOutputPath(ci, inFile);
    }

    std::vector<std::string> collapsePaths = opts_.collapsePaths;
    LockTypeConfig lockCfg;
    lockCfg.userAllowlist = opts_.lockTypes;
    lockCfg.useBuiltins = opts_.useBuiltinLocks;
    ChannelTypeConfig channelCfg = channelCfg_;
    mergeExtensionConfig(orgCfg_, lockCfg, channelCfg, collapsePaths);

    return std::make_unique<MegascopePluginConsumer>(
        std::move(tuPath), std::move(outPath), opts_.emitShardPath,
        opts_.emitMgscPath, std::move(collapsePaths), opts_.entryPoints,
        std::move(lockCfg), std::move(channelCfg), opts_.noWait, opts_.durable,
        opts_.verbose);
  }

private:
  PluginOptions opts_;
  ChannelTypeConfig channelCfg_;
  OrgConfig orgCfg_;
};

} // namespace
} // namespace vycor

static clang::FrontendPluginRegistry::Add<vycor::MegascopePluginAction>
    X("vycor-megascope",
      "Emit a vycor megascope (.vycs) call-graph and control-flow shard "
      "during compilation");
