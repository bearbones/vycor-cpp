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

// `megascope diff` and the impact-of-change patch adapter
// (docs/change-impact.md). The diff loads two saved indexes read-only,
// runs the semantic diff, and optionally a route diff and an impact walk
// over the after index; every payload is completed against the after
// index's facts so `indexScope` names what the answer was read from.

#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/cli/MegascopeCli.h"
#include "vycor/query/ChangeImpact.h"
#include "vycor/query/Identity.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace vycor {

namespace {

struct DiffOpts {
  std::string before, after;
  std::string to, toUsr, from, fromUsr;
  bool impact = false;
  unsigned maxDepth = 0; // 0: the verb's defaults (routes 20, impact 10)
  bool maxDepthSet = false;
  unsigned maxPaths = 50;
  size_t maxResults = 200;
  size_t maxFanIn = 1000;
  size_t maxSites = 8;
  bool noContext = false;
  bool includeMoves = false;
  bool allowMismatch = false;
};

// --key value | --key=value | bare boolean. Underscores read as hyphens.
llvm::Expected<DiffOpts> parseDiffFlags(llvm::ArrayRef<std::string> argv) {
  DiffOpts o;
  for (size_t i = 0; i < argv.size(); ++i) {
    llvm::StringRef a = argv[i];
    if (!a.starts_with("--"))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unexpected argument '%s'",
                                     a.str().c_str());
    auto [rawKey, inlineVal] = a.drop_front(2).split('=');
    const bool hasInline = a.contains('=');
    std::string key = rawKey.str();
    std::replace(key.begin(), key.end(), '_', '-');
    bool *flag = nullptr;
    if (key == "impact")
      flag = &o.impact;
    else if (key == "no-context")
      flag = &o.noContext;
    else if (key == "include-moves")
      flag = &o.includeMoves;
    else if (key == "allow-mismatch")
      flag = &o.allowMismatch;
    if (flag) {
      if (hasInline)
        return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                       "--%s takes no value", key.c_str());
      *flag = true;
      continue;
    }
    std::string *target = nullptr;
    unsigned *num = nullptr;
    size_t *size = nullptr;
    if (key == "before")
      target = &o.before;
    else if (key == "after")
      target = &o.after;
    else if (key == "to")
      target = &o.to;
    else if (key == "to-usr")
      target = &o.toUsr;
    else if (key == "from")
      target = &o.from;
    else if (key == "from-usr")
      target = &o.fromUsr;
    else if (key == "max-depth")
      num = &o.maxDepth;
    else if (key == "max-paths")
      num = &o.maxPaths;
    else if (key == "max-results")
      size = &o.maxResults;
    else if (key == "max-fan-in")
      size = &o.maxFanIn;
    else if (key == "max-sites")
      size = &o.maxSites;
    else
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "unknown flag '%s'", a.str().c_str());
    llvm::StringRef value;
    if (hasInline) {
      value = inlineVal;
    } else if (i + 1 < argv.size()) {
      value = argv[++i];
    } else {
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "--%s needs a value", key.c_str());
    }
    if (target) {
      *target = value.str();
      continue;
    }
    unsigned long long n = 0;
    if (value.getAsInteger(10, n))
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "--%s: '%s' is not a non-negative "
                                     "integer",
                                     key.c_str(), value.str().c_str());
    if (num) {
      *num = static_cast<unsigned>(n);
      if (key == "max-depth")
        o.maxDepthSet = true;
    } else {
      *size = static_cast<size_t>(n);
    }
  }
  return o;
}

struct LoadedSide {
  SnapshotData *snap = nullptr; // leaked on purpose (one-shot process)
  std::optional<ControlFlowOracle> oracle;
  QueryCache cache;
  std::vector<std::string> entryPoints;
  std::optional<ToolContext> ctx;
  std::optional<ContextSignatures> signatures;
};

int loadSide(const std::string &path, unsigned needs,
             const std::vector<std::string> &queryEntryPoints,
             const char *label, LoadedSide &side, llvm::raw_ostream &err) {
  if (!llvm::sys::fs::exists(path)) {
    err << "megascope diff: no index at " << path << " (" << label << ")\n";
    return kExitIndex;
  }
  SnapshotLoadStats stats;
  auto *holder = new std::optional<SnapshotData>(
      SnapshotIO::load(path, &stats, LoadMode::ReadOnly, needs));
  if (!*holder) {
    err << "megascope diff: cannot load index " << path << " (" << label
        << "): " << stats.error << "\n";
    return kExitIndex;
  }
  side.snap = &**holder;
  side.oracle.emplace(side.snap->graph, side.snap->cfIndex);
  side.entryPoints = queryEntryPoints;
  if (side.entryPoints.empty())
    side.entryPoints = side.snap->meta.entryPoints;
  if (side.entryPoints.empty())
    side.entryPoints.push_back("main");
  side.ctx.emplace(ToolContext{side.snap->graph, *side.oracle,
                               side.snap->cfIndex, side.entryPoints,
                               &side.snap->channels, &side.cache,
                               &side.snap->summary});
  side.ctx->facts =
      IndexFacts::of(side.snap->meta, IndexFreshness::Unchecked);
  if (needs & kSectionControlFlow) {
    // One pass over the contexts, then the section is dropped: the two
    // sides never hold their control-flow indexes at once (the oracle
    // and context keep referring to the now-empty member).
    side.signatures = ContextSignatures::build(side.snap->cfIndex);
    side.snap->cfIndex = ControlFlowIndex();
  }
  return kExitResults;
}

std::string joinReasons(const std::vector<std::string> &reasons) {
  std::string out;
  for (const auto &r : reasons) {
    if (!out.empty())
      out += "; ";
    out += r;
  }
  return out;
}

} // namespace

int runDiffVerb(llvm::ArrayRef<std::string> args, OutputFormat format,
                bool pretty, const std::vector<std::string> &entryPoints,
                llvm::raw_ostream &out, llvm::raw_ostream &err) {
  auto opts = parseDiffFlags(args);
  if (!opts) {
    err << "megascope diff: " << llvm::toString(opts.takeError()) << "\n";
    return kExitUsage;
  }
  if (opts->before.empty() || opts->after.empty()) {
    err << "megascope diff: --before <index> and --after <index> are "
           "required\n";
    return kExitUsage;
  }
  if (!opts->to.empty() && !opts->toUsr.empty()) {
    err << "megascope diff: pass --to or --to-usr, not both\n";
    return kExitUsage;
  }
  if (!opts->from.empty() && !opts->fromUsr.empty()) {
    err << "megascope diff: pass --from or --from-usr, not both\n";
    return kExitUsage;
  }
  const unsigned needs =
      kSectionGraph | (opts->noContext ? 0u : kSectionControlFlow);
  LoadedSide before, after;
  if (int rc = loadSide(opts->before, needs, entryPoints, "--before", before,
                        err))
    return rc;
  if (int rc =
          loadSide(opts->after, needs, entryPoints, "--after", after, err))
    return rc;

  DiffSide sideB{before.snap->graph, nullptr, &before.snap->meta,
                 before.signatures ? &*before.signatures : nullptr};
  DiffSide sideA{after.snap->graph, nullptr, &after.snap->meta,
                 after.signatures ? &*after.signatures : nullptr};
  SemanticDiffOptions diffOpts;
  diffOpts.compareContexts = !opts->noContext;
  diffOpts.includeMoves = opts->includeMoves;
  diffOpts.maxSites = opts->maxSites;
  diffOpts.allowMismatch = opts->allowMismatch;
  IdentityTable idB, idA;
  SemanticDiffResult diff = semanticDiff(sideB, sideA, diffOpts, &idB, &idA);

  if (diff.refused) {
    llvm::json::Value payload = unavailableError(
        "indexes are not comparable (" +
        joinReasons(diff.comparability.reasons) +
        "); pass --allow-mismatch to diff them anyway");
    (*payload.getAsObject())["comparability"] =
        serializeComparability(diff.comparability);
    payload = completeResult(std::move(payload), *after.ctx);
    return emitToolResult(payload, "changes", format, pretty, out, err,
                          "diff");
  }

  llvm::json::Value payload = serializeSemanticDiff(diff);
  llvm::json::Object &obj = *payload.getAsObject();

  const std::string target = !opts->toUsr.empty() ? opts->toUsr : opts->to;
  if (!target.empty()) {
    // A display name shared by several functions on either side is the
    // same ambiguity every tool reports; the client picks a usr.
    if (opts->toUsr.empty()) {
      auto usrsB = before.snap->graph.usrsForName(target);
      auto usrsA = after.snap->graph.usrsForName(target);
      if (usrsB.size() > 1 || usrsA.size() > 1) {
        const bool onAfter = usrsA.size() > 1;
        llvm::json::Value amb = makeAmbiguousNameResult(
            onAfter ? *after.ctx : *before.ctx, "to", target,
            onAfter ? std::move(usrsA) : std::move(usrsB));
        (*amb.getAsObject())["side"] = onAfter ? "after" : "before";
        amb = completeResult(std::move(amb), *after.ctx);
        return emitToolResult(amb, "changes", format, pretty, out, err,
                              "diff");
      }
    }
    std::vector<std::string> startsB = before.entryPoints;
    std::vector<std::string> startsA = after.entryPoints;
    if (!opts->fromUsr.empty())
      startsB = startsA = {opts->fromUsr};
    else if (!opts->from.empty())
      startsB = startsA = {opts->from};
    SearchLimits limits;
    limits.maxPaths = opts->maxPaths;
    limits.maxDepth = opts->maxDepthSet ? opts->maxDepth : 20;
    limits.maxFanIn = opts->maxFanIn;
    RouteDiff routes = diffRoutes(sideB, sideA, idB, idA, target, startsB,
                                  startsA, limits);
    obj["routes"] = serializeRouteDiff(routes, target);
  }

  if (opts->impact) {
    ImpactLimits limits;
    limits.maxDepth = opts->maxDepthSet ? opts->maxDepth : 10;
    limits.maxFanIn = opts->maxFanIn;
    std::vector<std::string> changed = changedFunctionsAfter(diff, idA);
    ImpactResult impact = findImpact(after.snap->graph, changed, limits);
    llvm::json::Value imp = serializeImpact(impact, after.snap->graph,
                                            after.entryPoints,
                                            opts->maxResults, true);
    if (auto *changedArr = imp.getAsObject()->getArray("changed"))
      for (auto &entry : *changedArr)
        if (auto *eo = entry.getAsObject())
          (*eo)["via"] = mappingViaName(MappingVia::Diff);
    obj["impact"] = std::move(imp);
  }

  payload = completeResult(std::move(payload), *after.ctx);
  return emitToolResult(payload, "changes", format, pretty, out, err,
                        "diff");
}

// ---------------------------------------------------------------------------
// impact-of-change --patch-file / --git-base / --git-head / --repo
// ---------------------------------------------------------------------------

namespace {

bool takeFlag(std::vector<std::string> &args, llvm::StringRef name,
              std::string &value, bool &present, std::string &error) {
  for (size_t i = 0; i < args.size(); ++i) {
    llvm::StringRef a = args[i];
    std::string key = a.starts_with("--") ? a.drop_front(2).str() : "";
    std::replace(key.begin(), key.end(), '_', '-');
    llvm::StringRef k(key);
    auto [bare, inlineVal] = k.split('=');
    if (bare != name)
      continue;
    present = true;
    if (k.contains('=')) {
      value = inlineVal.str();
      args.erase(args.begin() + static_cast<std::ptrdiff_t>(i));
      return true;
    }
    if (i + 1 >= args.size()) {
      error = "--" + name.str() + " needs a value";
      return false;
    }
    value = args[i + 1];
    args.erase(args.begin() + static_cast<std::ptrdiff_t>(i),
               args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
    return true;
  }
  return true;
}

} // namespace

int seedImpactPatch(std::vector<std::string> &args, llvm::json::Object &seed,
                    std::istream &in, llvm::raw_ostream &err) {
  std::string patchFile, gitBase, gitHead, repo, error;
  bool hasFile = false, hasBase = false, hasHead = false, hasRepo = false;
  if (!takeFlag(args, "patch-file", patchFile, hasFile, error) ||
      !takeFlag(args, "git-base", gitBase, hasBase, error) ||
      !takeFlag(args, "git-head", gitHead, hasHead, error) ||
      !takeFlag(args, "repo", repo, hasRepo, error)) {
    err << "megascope impact-of-change: " << error << "\n";
    return kExitUsage;
  }
  if (!hasFile && !hasBase && !hasHead && !hasRepo)
    return kExitResults;
  if (hasFile && (hasBase || hasHead)) {
    err << "megascope impact-of-change: --patch-file and --git-base/"
           "--git-head are alternatives\n";
    return kExitUsage;
  }
  if (hasBase != hasHead) {
    err << "megascope impact-of-change: --git-base and --git-head go "
           "together\n";
    return kExitUsage;
  }
  if (hasRepo && !hasBase) {
    err << "megascope impact-of-change: --repo only applies with "
           "--git-base/--git-head\n";
    return kExitUsage;
  }
  std::string patch;
  if (hasFile) {
    if (patchFile == "-") {
      patch.assign(std::istreambuf_iterator<char>(in),
                   std::istreambuf_iterator<char>());
    } else {
      auto buf = llvm::MemoryBuffer::getFile(patchFile);
      if (!buf) {
        err << "megascope impact-of-change: cannot read " << patchFile
            << ": " << buf.getError().message() << "\n";
        return kExitUsage;
      }
      patch = (*buf)->getBuffer().str();
    }
  } else {
    auto git = llvm::sys::findProgramByName("git");
    if (!git) {
      err << "megascope impact-of-change: git not found on PATH\n";
      return kExitUsage;
    }
    llvm::SmallString<128> tmp;
    if (auto ec = llvm::sys::fs::createTemporaryFile("vycor-diff", "patch",
                                                     tmp)) {
      err << "megascope impact-of-change: cannot create a temporary file: "
          << ec.message() << "\n";
      return kExitUsage;
    }
    std::vector<llvm::StringRef> argv = {*git};
    if (hasRepo) {
      argv.push_back("-C");
      argv.push_back(repo);
    }
    argv.push_back("diff");
    argv.push_back("-U0");
    argv.push_back("--no-color");
    argv.push_back("--no-ext-diff");
    argv.push_back(gitBase);
    argv.push_back(gitHead);
    argv.push_back("--");
    std::string errMsg;
    llvm::StringRef tmpRef(tmp);
    std::optional<llvm::StringRef> redirects[] = {std::nullopt, tmpRef,
                                                  std::nullopt};
    int rc = llvm::sys::ExecuteAndWait(*git, argv, std::nullopt, redirects,
                                       0, 0, &errMsg);
    auto buf = llvm::MemoryBuffer::getFile(tmp);
    llvm::sys::fs::remove(tmp);
    if (rc != 0 || !buf) {
      err << "megascope impact-of-change: `git diff " << gitBase << " "
          << gitHead << "` failed";
      if (!errMsg.empty())
        err << ": " << errMsg;
      err << " (exit " << rc << ")\n";
      return kExitUsage;
    }
    patch = (*buf)->getBuffer().str();
  }
  seed["patch"] = patch;
  return kExitResults;
}

} // namespace vycor
