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

#include "vycor/callgraph/MegascopeBridge.h"
#include "vycor/callgraph/AtomicFile.h"

#if defined(VYCOR_HAS_LIBMEGASCOPE)
#include "megascope.h"
#endif

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace vycor {

namespace {

constexpr char kMegascopeMagic[8] = {'P', 'Y', 'R', 'X', 0, 1, 0, 0};

// Mask 64-bit unsigned hashes to 63 bits so llvm::json::Value (which stores
// integers as signed int64_t) never serializes a negative integer for a Rust
// u64 field.
int64_t toJsonU64(uint64_t val) {
  return static_cast<int64_t>(val & 0x7fffffffffffffffULL);
}

uint64_t fnv1a64(llvm::StringRef s, uint64_t seed = 14695981039346656037ULL) {
  uint64_t h = seed;
  for (unsigned char c : s) {
    h ^= static_cast<uint64_t>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

uint64_t parseHexOrHash(llvm::StringRef fp, llvm::StringRef fallback) {
  if (!fp.empty()) {
    uint64_t val = 0;
    llvm::StringRef prefix = fp.substr(0, std::min<size_t>(fp.size(), 16));
    if (!prefix.getAsInteger(16, val) && val != 0)
      return val;
    return fnv1a64(fp);
  }
  return fnv1a64(fallback);
}

// Parse "file:line:col" or "file:line" from the right.
void parseLocation(llvm::StringRef loc, std::string &fileOut, uint32_t &lineOut,
                   uint32_t &colOut) {
  fileOut = loc.str();
  lineOut = 0;
  colOut = 0;
  if (loc.empty())
    return;

  auto parseTrailingUint = [](llvm::StringRef s, size_t &colonPos,
                              uint32_t &numOut) -> bool {
    size_t pos = s.rfind(':');
    if (pos == llvm::StringRef::npos || pos + 1 >= s.size())
      return false;
    llvm::StringRef tail = s.substr(pos + 1);
    for (char c : tail) {
      if (!std::isdigit(static_cast<unsigned char>(c)))
        return false;
    }
    unsigned long long v = 0;
    if (tail.getAsInteger(10, v))
      return false;
    colonPos = pos;
    numOut = static_cast<uint32_t>(v);
    return true;
  };

  size_t lastColon = 0;
  uint32_t n1 = 0;
  if (!parseTrailingUint(loc, lastColon, n1))
    return;

  llvm::StringRef beforeLast = loc.substr(0, lastColon);
  size_t secondLastColon = 0;
  uint32_t n2 = 0;
  if (parseTrailingUint(beforeLast, secondLastColon, n2)) {
    fileOut = beforeLast.substr(0, secondLastColon).str();
    lineOut = n2;
    colOut = n1;
  } else {
    fileOut = beforeLast.str();
    lineOut = n1;
    colOut = 0;
  }
}

std::string shortNameOf(llvm::StringRef qualifiedName) {
  if (qualifiedName.empty())
    return "";
  // Strip trailing parameter list if any, keeping balanced parens in mind
  llvm::StringRef head = qualifiedName;
  size_t parenPos = head.find('(');
  if (parenPos != llvm::StringRef::npos && parenPos > 0 &&
      !head.substr(0, parenPos).ends_with("operator")) {
    head = head.substr(0, parenPos);
  }
  size_t pos = head.rfind("::");
  if (pos != llvm::StringRef::npos && pos + 2 < head.size())
    return head.substr(pos + 2).str();
  size_t dotPos = head.rfind('.');
  if (dotPos != llvm::StringRef::npos && dotPos + 1 < head.size())
    return head.substr(dotPos + 1).str();
  return head.str();
}

const char *mapEdgeKind(EdgeKind kind) {
  switch (kind) {
  case EdgeKind::DirectCall:
    return "direct";
  case EdgeKind::VirtualDispatch:
    return "dynamic_dispatch";
  case EdgeKind::FunctionPointer:
    return "function_pointer";
  case EdgeKind::ConstructorCall:
    return "constructor";
  case EdgeKind::DestructorCall:
    return "destructor";
  case EdgeKind::OperatorCall:
    return "operator_call";
  case EdgeKind::TemplateInstantiation:
    return "template_instantiation";
  case EdgeKind::LambdaCall:
    return "lambda_call";
  case EdgeKind::ThreadEntry:
    return "thread_entry";
  case EdgeKind::FunctionPointerReturn:
    return "function_pointer_return";
  }
  return "direct";
}

const char *mapConfidence(Confidence c) {
  switch (c) {
  case Confidence::Proven:
    return "proven";
  case Confidence::Plausible:
    return "plausible";
  case Confidence::Unknown:
    return "dynamic";
  }
  return "proven";
}

const char *mapExecContext(ExecutionContext ec) {
  switch (ec) {
  case ExecutionContext::Synchronous:
    return "sync";
  case ExecutionContext::ThreadSpawn:
    return "thread_spawn";
  case ExecutionContext::AsyncTask:
    return "async";
  case ExecutionContext::PackagedTask:
    return "packaged_task";
  case ExecutionContext::Invoke:
    return "invoke";
  }
  return "sync";
}

bool isResourceOrLockRaii(const RaiiLocal &raii) {
  if (raii.kind == RaiiKind::Lock)
    return true;
  if (raii.kind == RaiiKind::SmartPtr)
    return false;

  // Strip template parameters so `std::vector<LockGuard>` is judged by its
  // outer container type (`std::vector`), not its element type.
  llvm::StringRef head(raii.typeName);
  size_t ltPos = head.find('<');
  if (ltPos != llvm::StringRef::npos)
    head = head.substr(0, ltPos);
  head = head.trim();

  // Exclude standard value containers, strings, formatting buffers, and
  // polymorphic wrappers.
  std::string lower = head.lower();
  static const char *const kExcludedSubstrings[] = {
      "basic_string",   "string",         "vector",       "smallvector",
      "smallstring",    "map",            "set",          "unordered_",
      "densemap",       "denseset",       "stringmap",    "stringset",
      "optional",       "variant",        "any",          "tuple",
      "pair",           "array",          "deque",        "list",
      "function",       "unique_ptr",     "shared_ptr",   "weak_ptr",
      "intrusive_ref",  "raw_string_ostream", " raw_svector_ostream",
      "stringstream",   "ostringstream",  "istringstream","regex",
      "path",           "twine",          "error",        "expected",
      "json::",
  };
  for (const char *ex : kExcludedSubstrings) {
    if (lower.find(ex) != std::string::npos)
      return false;
  }

  static const char *const kIncludedSubstrings[] = {
      "lock",           "mutex",          "guard",        "scoped",
      "scope_exit",     "scope_fail",     "scope_success","cleanup",
      "defer",          "finally",        "final_act",    "saveandrestore",
      "save_and_restore","latch",         "barrier",      "semaphore",
      "critical_section","transaction",   "session",      "ifstream",
      "ofstream",       "fstream",        "raw_fd_ostream","tooloutputfile",
      "tempfile",       "fileremover",
  };
  for (const char *inc : kIncludedSubstrings) {
    if (lower.find(inc) != std::string::npos)
      return true;
  }
  return false;
}

const char *classifyRaiiResourceKind(const RaiiLocal &raii) {
  if (raii.kind == RaiiKind::Lock)
    return "threading_lock";
  llvm::StringRef head(raii.typeName);
  size_t ltPos = head.find('<');
  if (ltPos != llvm::StringRef::npos)
    head = head.substr(0, ltPos);
  std::string lower = head.lower();
  if (lower.find("lock") != std::string::npos ||
      lower.find("mutex") != std::string::npos ||
      lower.find("critical_section") != std::string::npos ||
      lower.find("semaphore") != std::string::npos) {
    return "threading_lock";
  }
  if (lower.find("transaction") != std::string::npos)
    return "transaction";
  if (lower.find("session") != std::string::npos)
    return "http_session";
  if (lower.find("temp") != std::string::npos)
    return "tempfile";
  if (lower.find("fstream") != std::string::npos ||
      lower.find("raw_fd_ostream") != std::string::npos ||
      lower.find("file") != std::string::npos) {
    return "file_handle";
  }
  return "other";
}

const char *mapParseOutcome(TuStatus status) {
  switch (status) {
  case TuStatus::Indexed:
    return "indexed";
  case TuStatus::Partial:
    return "partial";
  case TuStatus::Crashed:
  case TuStatus::Poisoned:
  case TuStatus::Skipped:
  case TuStatus::TimedOut:
    return "failed";
  }
  return "indexed";
}

llvm::json::Object guardToJson(const ConditionalGuard &g) {
  std::string file;
  uint32_t line = 0, col = 0;
  parseLocation(g.location, file, line, col);
  llvm::json::Object obj;
  obj["kind"] = g.isAssertion ? "assert_guard" : "condition";
  obj["expr"] = g.conditionText;
  obj["in_true_branch"] = g.inTrueBranch;
  obj["line"] = static_cast<int64_t>(line);
  return obj;
}

llvm::json::Object makeEntityNodeJson(
    const std::string &usr, const std::string &qualifiedName,
    const std::string &shortName, const std::string &moduleName,
    const std::string &enclosingClass, const std::string &file, uint32_t line,
    const char *kindStr, bool isEntryPoint, bool isAbstract,
    const std::vector<std::string> &bases, uint64_t skeletonHash,
    const std::string &normalizedSkeleton) {
  llvm::json::Object ent;
  ent["usr"] = usr;
  ent["qualified_name"] = qualifiedName;
  ent["name"] = shortName;
  ent["module"] = moduleName;
  if (enclosingClass.empty())
    ent["enclosing_class"] = nullptr;
  else
    ent["enclosing_class"] = enclosingClass;
  ent["file"] = file;
  ent["line"] = static_cast<int64_t>(line);
  ent["column"] = static_cast<int64_t>(1);
  ent["end_line"] = static_cast<int64_t>(line);
  ent["end_column"] = static_cast<int64_t>(1);
  ent["byte_start"] = static_cast<int64_t>(0);
  ent["byte_end"] = static_cast<int64_t>(0);
  ent["kind"] = kindStr;
  ent["execution_context"] = "sync";
  ent["decorators"] = llvm::json::Array();
  ent["parameters"] = llvm::json::Array();
  ent["return_annotation"] = nullptr;

  llvm::json::Array basesArr;
  for (const auto &b : bases)
    basesArr.push_back(b);
  ent["bases"] = std::move(basesArr);
  ent["mro"] = llvm::json::Array();
  ent["is_protocol"] = false;
  ent["is_abc"] = isAbstract;
  ent["is_abstractmethod"] = isAbstract;
  ent["is_exported"] = true;
  ent["is_entry_point"] = isEntryPoint;
  if (isEntryPoint)
    ent["entry_point_kind"] = "main";
  else
    ent["entry_point_kind"] = nullptr;
  ent["skeleton_hash"] = toJsonU64(skeletonHash);
  ent["skeleton_features"] = llvm::json::Array();
  ent["normalized_skeleton"] = normalizedSkeleton;
  ent["statement_count"] = static_cast<int64_t>(1);
  ent["raises"] = llvm::json::Array();
  ent["broad_catches"] = llvm::json::Array();
  ent["returned_resources"] = llvm::json::Array();
  ent["dict_returns"] = llvm::json::Array();
  ent["dict_accesses"] = llvm::json::Array();
  ent["returns_optional_or_status"] = false;
  return ent;
}

} // namespace

bool MegascopeBridge::isMegascopeSnapshotPath(llvm::StringRef path) {
  if (path.ends_with(".mgsc") || path.ends_with(".pyrx"))
    return true;
  auto bufOrErr = llvm::MemoryBuffer::getFileSlice(path, 8, 0);
  if (!bufOrErr)
    return false;
  llvm::StringRef bytes = (*bufOrErr)->getBuffer();
  return bytes.size() >= 8 &&
         std::memcmp(bytes.data(), kMegascopeMagic, 8) == 0;
}

bool MegascopeBridge::isMegascopeShardJsonPath(llvm::StringRef path) {
  return path.ends_with(".shard.json") || path.ends_with(".shards.json");
}

llvm::json::Object MegascopeBridge::toShardObject(
    const CallGraph &graph, const ControlFlowIndex &cfIndex,
    const ChannelIndex &channels, const SnapshotMeta &meta,
    llvm::StringRef tuFilter) {
  std::string moduleFile = tuFilter.str();
  uint64_t fingerprint = 0;
  const char *outcomeStr = "indexed";
  std::string errorDetail;

  if (!meta.files.empty()) {
    size_t matchedIdx = meta.files.size();
    if (!tuFilter.empty()) {
      for (size_t i = 0; i < meta.files.size(); ++i) {
        if (meta.files[i].path == tuFilter) {
          matchedIdx = i;
          break;
        }
      }
    } else {
      matchedIdx = 0;
    }

    if (matchedIdx < meta.files.size()) {
      if (moduleFile.empty())
        moduleFile = meta.files[matchedIdx].path;
      std::string fpStr = matchedIdx < meta.fingerprints.size()
                              ? meta.fingerprints[matchedIdx]
                              : "";
      fingerprint = parseHexOrHash(fpStr, moduleFile);
      if (matchedIdx < meta.outcomes.size()) {
        outcomeStr = mapParseOutcome(meta.outcomes[matchedIdx].status);
        errorDetail = meta.outcomes[matchedIdx].detail;
      }
    }
  }
  if (moduleFile.empty())
    moduleFile = "<cpp_tu>";
  if (fingerprint == 0)
    fingerprint = fnv1a64(moduleFile);

  std::string moduleName = moduleFile;

  // Determine TU filter SId in CallGraph if present
  std::optional<CallGraph::SId> tuIdOpt;
  std::unordered_set<size_t> allowedEdgeIndices;
  std::unordered_set<CallGraph::SId> allowedNodeIds;
  const bool filterByTu = !tuFilter.empty();
  if (filterByTu) {
    tuIdOpt = graph.findText(tuFilter.str());
    if (tuIdOpt) {
      auto itE = graph.tuEdges_.find(*tuIdOpt);
      if (itE != graph.tuEdges_.end()) {
        allowedEdgeIndices.insert(itE->second.begin(), itE->second.end());
      }
      auto itN = graph.tuNodes_.find(*tuIdOpt);
      if (itN != graph.tuNodes_.end()) {
        allowedNodeIds.insert(itN->second.begin(), itN->second.end());
      }
    }
  }

  auto edgeMatchesTu = [&](size_t edgeIdx,
                           const CallGraph::StoredEdge &se) -> bool {
    if (!filterByTu)
      return true;
    if (!graph.tuEdges_.empty())
      return allowedEdgeIndices.find(edgeIdx) != allowedEdgeIndices.end();
    std::string eFile;
    uint32_t eLine = 0, eCol = 0;
    parseLocation(graph.interner_.resolve(se.callSite), eFile, eLine, eCol);
    return eFile == tuFilter;
  };

  // Collect per-caller outgoing callees for deterministic skeleton hashing
  std::unordered_map<CallGraph::SId, std::vector<std::string>> calleesByCaller;
  {
    std::unordered_set<uint64_t> seenCallerCallee;
    seenCallerCallee.reserve(graph.edges_.size() / 4 + 16);
    for (size_t i = 0; i < graph.edges_.size(); ++i) {
      const auto &se = graph.edges_[i];
      if (se.refs == 0)
        continue;
      if (!edgeMatchesTu(i, se))
        continue;
      uint64_t pairKey =
          (static_cast<uint64_t>(se.caller) << 32) | se.callee;
      if (seenCallerCallee.insert(pairKey).second)
        calleesByCaller[se.caller].push_back(graph.displayFor(se.callee));
    }
  }

  // Build class hierarchy map: derivedClass -> vector<baseClass>
  std::map<std::string, std::vector<std::string>> classBases;
  std::set<std::string> allClasses;
  for (const auto &[baseId, derivedIds] : graph.derivedClasses_) {
    const std::string &baseName = graph.displayFor(baseId);
    if (!filterByTu)
      allClasses.insert(baseName);
    for (CallGraph::SId dId : derivedIds) {
      const std::string &derivedName = graph.displayFor(dId);
      if (!filterByTu)
        allClasses.insert(derivedName);
      classBases[derivedName].push_back(baseName);
    }
  }

  llvm::json::Array entitiesArr;
  std::set<std::string> emittedClassNames;
  std::set<std::string> classesWithAnyMethods;
  std::map<std::string, std::pair<std::string, uint32_t>> classLocations;

  auto addClassAndBases = [&](const std::string &cls,
                              std::set<std::string> &target) {
    std::vector<std::string> stack = {cls};
    while (!stack.empty()) {
      std::string cur = std::move(stack.back());
      stack.pop_back();
      if (cur.empty() || !target.insert(cur).second)
        continue;
      auto itBases = classBases.find(cur);
      if (itBases != classBases.end()) {
        for (const auto &b : itBases->second)
          stack.push_back(b);
      }
    }
  };

  auto isLambdaNode = [](const CallGraphNode &node) {
    const std::string &usr =
        node.usr.empty() ? node.qualifiedName : node.usr;
    const std::string &qname =
        node.qualifiedName.empty() ? usr : node.qualifiedName;
    return usr.rfind("vycor-lambda:", 0) == 0 ||
           qname.rfind("lambda#", 0) == 0 ||
           qname.find("(lambda") != std::string::npos;
  };

  for (const auto &[usrId, node] : graph.nodes_) {
    if (!node.enclosingClass.empty() && !isLambdaNode(node)) {
      addClassAndBases(node.enclosingClass, classesWithAnyMethods);
      if (!node.file.empty() && node.file != "<unknown>" && node.line > 0) {
        auto it = classLocations.find(node.enclosingClass);
        if (it == classLocations.end() ||
            std::tie(node.file, node.line) <
                std::tie(it->second.first, it->second.second)) {
          classLocations[node.enclosingClass] = {node.file, node.line};
        }
      }
    }
  }

  // Sort nodes deterministically by USR
  std::vector<std::pair<CallGraph::SId, const CallGraphNode *>> sortedNodes;
  sortedNodes.reserve(graph.nodes_.size());
  std::map<std::string, std::pair<std::string, uint32_t>> tuClassLocations;
  for (const auto &[usrId, node] : graph.nodes_) {
    if (filterByTu) {
      const bool inTuNodes =
          !graph.tuNodes_.empty() &&
          allowedNodeIds.find(usrId) != allowedNodeIds.end();
      const bool inTuFile = (node.file == tuFilter);
      if (!inTuNodes && !inTuFile)
        continue;
    }
    sortedNodes.push_back({usrId, &node});
    if (!node.enclosingClass.empty() && !isLambdaNode(node)) {
      addClassAndBases(node.enclosingClass, allClasses);
      if (!node.file.empty() && node.file != "<unknown>" && node.line > 0) {
        auto it = tuClassLocations.find(node.enclosingClass);
        if (it == tuClassLocations.end() ||
            std::tie(node.file, node.line) <
                std::tie(it->second.first, it->second.second)) {
          tuClassLocations[node.enclosingClass] = {node.file, node.line};
        }
      }
    }
  }
  if (filterByTu && !sortedNodes.empty()) {
    // Include methodless classes from derivedClasses_ so pure hierarchy entries
    // without method nodes are not lost.
    for (const auto &[baseId, derivedIds] : graph.derivedClasses_) {
      const std::string &baseName = graph.displayFor(baseId);
      if (!classesWithAnyMethods.count(baseName))
        addClassAndBases(baseName, allClasses);
      for (CallGraph::SId dId : derivedIds) {
        const std::string &derivedName = graph.displayFor(dId);
        if (!classesWithAnyMethods.count(derivedName))
          addClassAndBases(derivedName, allClasses);
      }
    }
  }
  std::sort(sortedNodes.begin(), sortedNodes.end(),
            [](const auto &a, const auto &b) {
              return a.second->usr < b.second->usr;
            });

  for (const auto &[usrId, nodePtr] : sortedNodes) {
    const CallGraphNode &node = *nodePtr;
    const std::string &usr =
        node.usr.empty() ? node.qualifiedName : node.usr;
    const std::string &qname =
        node.qualifiedName.empty() ? usr : node.qualifiedName;
    std::string sname = shortNameOf(qname);
    std::string file = node.file.empty() ? moduleFile : node.file;

    const bool isLambda = isLambdaNode(node);
    const std::string &effectiveEnclosingClass =
        isLambda ? std::string() : node.enclosingClass;
    const char *kindStr = "function";
    if (isLambda) {
      kindStr = "lambda";
    } else if (!effectiveEnclosingClass.empty()) {
      kindStr = "method";
    }

    // Method override bases
    std::vector<std::string> methodBases;
    auto itOb = graph.overrideBases_.find(usrId);
    if (itOb != graph.overrideBases_.end()) {
      for (CallGraph::SId baseMId : itOb->second) {
        methodBases.push_back(graph.displayFor(baseMId));
      }
      std::sort(methodBases.begin(), methodBases.end());
      methodBases.erase(std::unique(methodBases.begin(), methodBases.end()),
                        methodBases.end());
    }

    // Skeleton hash over signature + outgoing callees
    std::string skel = qname + "|" + (node.isVirtual ? "virtual" : "nonvirtual");
    auto itCalls = calleesByCaller.find(usrId);
    if (itCalls != calleesByCaller.end()) {
      std::vector<std::string> sortedCalls = itCalls->second;
      std::sort(sortedCalls.begin(), sortedCalls.end());
      sortedCalls.erase(std::unique(sortedCalls.begin(), sortedCalls.end()),
                        sortedCalls.end());
      for (const auto &c : sortedCalls) {
        skel += "->";
        skel += c;
      }
    }
    uint64_t skelHash = fnv1a64(skel);

    bool isEntry = node.isEntryPoint || qname == "main" || sname == "main";
    if (!isEntry) {
      for (const auto &ep : meta.entryPoints) {
        if (ep == qname || ep == usr || ep == sname) {
          isEntry = true;
          break;
        }
      }
    }

    if (!effectiveEnclosingClass.empty() && qname == effectiveEnclosingClass)
      emittedClassNames.insert(qname);

    entitiesArr.push_back(makeEntityNodeJson(
        usr, qname, sname, moduleName, effectiveEnclosingClass, file, node.line,
        kindStr, isEntry, false, methodBases, skelHash, skel));
  }

  // Emit Class entities for classes discovered in hierarchy or enclosingClass
  for (const auto &clsName : allClasses) {
    if (clsName.empty() || emittedClassNames.count(clsName))
      continue;
    std::vector<std::string> bases = classBases[clsName];
    std::sort(bases.begin(), bases.end());
    bases.erase(std::unique(bases.begin(), bases.end()), bases.end());
    std::string clsShort = shortNameOf(clsName);
    uint64_t skelHash = fnv1a64("class:" + clsName);
    std::string clsFile = moduleFile;
    uint32_t clsLine = 1;
    auto itTuLoc = tuClassLocations.find(clsName);
    if (itTuLoc != tuClassLocations.end()) {
      clsFile = itTuLoc->second.first;
      clsLine = itTuLoc->second.second;
    } else {
      auto itGlobalLoc = classLocations.find(clsName);
      if (itGlobalLoc != classLocations.end()) {
        clsFile = itGlobalLoc->second.first;
        clsLine = itGlobalLoc->second.second;
      }
    }
    entitiesArr.push_back(makeEntityNodeJson(
        "cpp-class:" + clsName, clsName, clsShort, moduleName, "", clsFile,
        clsLine, "class", false, false, bases, skelHash, "class " + clsName));
  }

  // Build raw_calls, raw_contexts, and lock_acquisitions.
  // Deduplicate template-instantiation duplicates at the same call site and
  // unguarded duplicate call sites between the same (callerDisplay,
  // calleeDisplay, kind, execContext).
  llvm::json::Array rawCallsArr;
  llvm::json::Array rawContextsArr;
  std::map<std::string, std::vector<std::tuple<std::string, uint32_t, bool>>>
      lockAcqs;

  std::set<std::tuple<std::string_view, std::string_view, CallGraph::SId,
                      uint8_t, uint8_t>>
      seenSiteEdges;
  std::set<std::tuple<std::string_view, std::string_view, uint8_t, uint8_t>>
      seenBareEdges;

  for (size_t i = 0; i < graph.edges_.size(); ++i) {
    const auto &se = graph.edges_[i];
    if (se.refs == 0)
      continue;
    if (!edgeMatchesTu(i, se))
      continue;

    const std::string &callerDisplay = graph.displayFor(se.caller);
    const std::string &calleeDisplay = graph.displayFor(se.callee);
    const uint8_t kindCode = static_cast<uint8_t>(se.kind);
    const uint8_t execCode = static_cast<uint8_t>(se.execContext);

    if (!seenSiteEdges
             .emplace(callerDisplay, calleeDisplay, se.callSite, kindCode,
                      execCode)
             .second) {
      continue;
    }

    const std::string &callerUsr = graph.interner_.resolve(se.caller);
    const std::string &calleeUsr = graph.interner_.resolve(se.callee);
    const std::string &callSiteStr = graph.interner_.resolve(se.callSite);

    // Look up control-flow context for this edge
    std::optional<CallSiteContext> cfOpt =
        cfIndex.contextForEdge(callSiteStr, callerUsr, calleeUsr);
    if (!cfOpt)
      cfOpt = cfIndex.contextAtSite(callSiteStr, callerUsr);
    if (!cfOpt)
      cfOpt = cfIndex.contextAtSite(callSiteStr);

    bool hasRelevantRaii = false;
    if (cfOpt) {
      for (const auto &raii : cfOpt->liveRaiiLocals) {
        if (isResourceOrLockRaii(raii)) {
          hasRelevantRaii = true;
          break;
        }
      }
    }
    const bool hasCf =
        cfOpt && (!cfOpt->enclosingTryCatches.empty() ||
                  hasRelevantRaii ||
                  !cfOpt->enclosingGuards.empty());
    if (!hasCf) {
      if (!seenBareEdges
               .emplace(callerDisplay, calleeDisplay, kindCode, execCode)
               .second) {
        continue;
      }
    }

    std::string edgeFile;
    uint32_t edgeLine = 0, edgeCol = 0;
    parseLocation(callSiteStr, edgeFile, edgeLine, edgeCol);
    if (edgeFile.empty())
      edgeFile = moduleFile;

    const char *execCtxStr = mapExecContext(se.execContext);
    uint32_t contextId = static_cast<uint32_t>(rawContextsArr.size());

    llvm::json::Array tryScopesArr;
    llvm::json::Array withScopesArr;
    llvm::json::Array guardsArr;

    if (cfOpt) {
      for (const auto &tc : cfOpt->enclosingTryCatches) {
        std::string tryFile;
        uint32_t tryLine = 0, tryCol = 0;
        parseLocation(tc.tryLocation, tryFile, tryLine, tryCol);

        llvm::json::Array handlersArr;
        for (const auto &h : tc.handlers) {
          std::string hFile;
          uint32_t hLine = 0, hCol = 0;
          parseLocation(h.location, hFile, hLine, hCol);

          llvm::json::Array caughtTypesArr;
          if (h.isCatchAll || h.caughtType.empty()) {
            caughtTypesArr.push_back("...");
          } else {
            caughtTypesArr.push_back(h.caughtType);
          }

          llvm::json::Object hObj;
          hObj["caught_types"] = std::move(caughtTypesArr);
          hObj["is_bare_except"] = h.isCatchAll;
          hObj["rethrows"] = h.rethrows;
          hObj["swallows"] = !h.rethrows;
          hObj["line"] = static_cast<int64_t>(hLine);
          handlersArr.push_back(std::move(hObj));
        }

        llvm::json::Object tryObj;
        tryObj["handlers"] = std::move(handlersArr);
        tryObj["has_finally"] = false;
        tryObj["line"] = static_cast<int64_t>(tryLine);
        tryScopesArr.push_back(std::move(tryObj));
      }

      for (const auto &raii : cfOpt->liveRaiiLocals) {
        if (!isResourceOrLockRaii(raii))
          continue;
        std::string rFile;
        uint32_t rLine = 0, rCol = 0;
        parseLocation(raii.declLocation, rFile, rLine, rCol);

        const char *resKind = classifyRaiiResourceKind(raii);
        llvm::json::Object wObj;
        wObj["context_expr"] = raii.typeName;
        if (raii.varName.empty())
          wObj["var_name"] = nullptr;
        else
          wObj["var_name"] = raii.varName;
        wObj["resource_kind"] = resKind;
        if (raii.kind == RaiiKind::Lock ||
            std::string_view(resKind) == "threading_lock") {
          std::string lockId =
              !raii.varName.empty() ? raii.varName : raii.typeName;
          wObj["lock_identity"] = lockId;
          lockAcqs[callerDisplay].push_back({lockId, rLine, false});
        } else {
          wObj["lock_identity"] = nullptr;
        }
        wObj["is_async"] = false;
        wObj["line"] = static_cast<int64_t>(rLine);
        withScopesArr.push_back(std::move(wObj));
      }

      for (const auto &g : cfOpt->enclosingGuards) {
        guardsArr.push_back(guardToJson(g));
      }
    }

    llvm::json::Object ctxObj;
    ctxObj["context_id"] = static_cast<int64_t>(contextId);
    ctxObj["caller"] = callerDisplay;
    ctxObj["callee"] = calleeDisplay;
    ctxObj["file"] = edgeFile;
    ctxObj["line"] = static_cast<int64_t>(edgeLine);
    ctxObj["column"] = static_cast<int64_t>(edgeCol);
    ctxObj["execution_context"] = execCtxStr;
    ctxObj["is_awaited"] = (se.execContext == ExecutionContext::AsyncTask);
    ctxObj["try_scopes"] = std::move(tryScopesArr);
    ctxObj["with_scopes"] = std::move(withScopesArr);
    ctxObj["guards"] = std::move(guardsArr);
    rawContextsArr.push_back(std::move(ctxObj));

    llvm::json::Object callObj;
    callObj["caller"] = callerDisplay;
    callObj["callee"] = calleeDisplay;
    callObj["callee_raw"] = calleeDisplay;
    callObj["module"] = moduleName;
    callObj["file"] = edgeFile;
    callObj["line"] = static_cast<int64_t>(edgeLine);
    callObj["column"] = static_cast<int64_t>(edgeCol);
    callObj["byte_start"] = static_cast<int64_t>(0);
    callObj["byte_end"] = static_cast<int64_t>(0);
    callObj["kind"] = mapEdgeKind(se.kind);
    callObj["confidence"] = mapConfidence(se.confidence);
    callObj["execution_context"] = execCtxStr;
    callObj["is_awaited"] = (se.execContext == ExecutionContext::AsyncTask);
    callObj["arg_constants"] = llvm::json::Array();
    callObj["passed_dict_keys"] = llvm::json::Array();
    callObj["context_id"] = static_cast<int64_t>(contextId);
    rawCallsArr.push_back(std::move(callObj));
  }

  // Channels
  llvm::json::Array channelsArr;
  for (const auto &site : channels.allSites()) {
    std::string cFile;
    uint32_t cLine = 0, cCol = 0;
    parseLocation(site.callSite, cFile, cLine, cCol);
    if (filterByTu) {
      if (!site.tuPath.empty()) {
        if (site.tuPath != tuFilter)
          continue;
      } else if (cFile != tuFilter) {
        continue;
      }
    }
    if (cFile.empty())
      cFile = moduleFile;

    llvm::json::Array siteGuardsArr;
    for (const auto &g : site.enclosingGuards)
      siteGuardsArr.push_back(guardToJson(g));

    llvm::json::Object chObj;
    chObj["channel_id"] = site.channelId;
    chObj["channel_type_name"] = site.channelTypeName;
    chObj["category"] = site.category;
    chObj["op"] =
        (site.op == ChannelOperation::Produce) ? "produce" : "consume";
    chObj["site_function_usr"] =
        site.siteFunctionUsr.empty() ? site.siteFunctionDisplay
                                     : site.siteFunctionUsr;
    chObj["site_function_display"] =
        site.siteFunctionDisplay.empty() ? site.siteFunctionUsr
                                         : site.siteFunctionDisplay;
    chObj["file"] = cFile;
    chObj["line"] = static_cast<int64_t>(cLine);
    chObj["column"] = static_cast<int64_t>(cCol);
    chObj["guards"] = std::move(siteGuardsArr);
    channelsArr.push_back(std::move(chObj));
  }

  // Function pointer returns
  llvm::json::Object fnReturnsObj;
  if (!filterByTu || !sortedNodes.empty() || !rawCallsArr.empty()) {
    for (const auto &[fnId, targetIds] : graph.functionReturns_) {
      const std::string &fnName = graph.displayFor(fnId);
      llvm::json::Array targetsArr;
      for (CallGraph::SId tId : targetIds)
        targetsArr.push_back(graph.displayFor(tId));
      fnReturnsObj[fnName] = std::move(targetsArr);
    }
  }

  // Lock acquisitions
  llvm::json::Object lockAcqsObj;
  for (auto &[fnName, list] : lockAcqs) {
    std::sort(list.begin(), list.end());
    list.erase(std::unique(list.begin(), list.end()), list.end());
    llvm::json::Array arr;
    for (const auto &[lockId, line, isAsync] : list) {
      llvm::json::Array tupleArr;
      tupleArr.push_back(lockId);
      tupleArr.push_back(static_cast<int64_t>(line));
      tupleArr.push_back(isAsync);
      arr.push_back(std::move(tupleArr));
    }
    lockAcqsObj[fnName] = std::move(arr);
  }

  llvm::json::Object shard;
  shard["module_name"] = moduleName;
  shard["file"] = moduleFile;
  shard["fingerprint"] = toJsonU64(fingerprint);
  shard["outcome"] = outcomeStr;
  if (errorDetail.empty())
    shard["error_detail"] = nullptr;
  else
    shard["error_detail"] = errorDetail;
  shard["entities"] = std::move(entitiesArr);
  shard["raw_calls"] = std::move(rawCallsArr);
  shard["raw_contexts"] = std::move(rawContextsArr);
  shard["imports"] = llvm::json::Array();
  shard["schemas"] = llvm::json::Array();
  shard["env_accesses"] = llvm::json::Array();
  shard["inline_suppressions"] = llvm::json::Array();
  shard["import_bindings"] = llvm::json::Object();
  shard["dunder_all"] = nullptr;
  shard["lock_acquisitions"] = std::move(lockAcqsObj);
  shard["await_under_threading_lock"] = llvm::json::Array();
  shard["channels"] = std::move(channelsArr);
  shard["function_returns"] = std::move(fnReturnsObj);
  return shard;
}

llvm::json::Array MegascopeBridge::toShardsArray(
    const CallGraph &graph, const ControlFlowIndex &cfIndex,
    const ChannelIndex &channels, const SnapshotMeta &meta) {
  llvm::json::Array arr;
  if (meta.files.size() <= 1) {
    arr.push_back(toShardObject(graph, cfIndex, channels, meta, ""));
    return arr;
  }

  // Build the deduplicated whole-graph shard once in O(V + E) and partition
  // its entities, calls/contexts, and channels across the per-TU shards so
  // shared header entities and edges are never duplicated N times across N TUs.
  llvm::json::Object full = toShardObject(graph, cfIndex, channels, meta, "");

  const size_t nTus = meta.files.size();
  std::vector<llvm::json::Array> tuEntities(nTus);
  std::vector<llvm::json::Array> tuCalls(nTus);
  std::vector<llvm::json::Array> tuContexts(nTus);
  std::vector<llvm::json::Array> tuChannels(nTus);
  std::unordered_map<std::string, size_t> tuByPath;
  size_t firstHealthy = nTus;

  for (size_t i = 0; i < nTus; ++i) {
    const char *outcomeStr =
        (i < meta.outcomes.size())
            ? mapParseOutcome(meta.outcomes[i].status)
            : "indexed";
    if ( std::string_view(outcomeStr) != "failed") {
      tuByPath.emplace(meta.files[i].path, i);
      if (firstHealthy == nTus)
        firstHealthy = i;
    }
  }

  // Map USR -> first healthy TU recorded in graph.tuNodes_
  std::unordered_map<std::string, size_t> usrOwnerTu;
  if (!graph.tuNodes_.empty()) {
    for (size_t i = 0; i < nTus; ++i) {
      if (tuByPath.find(meta.files[i].path) == tuByPath.end())
        continue;
      auto tuIdOpt = graph.findText(meta.files[i].path);
      if (!tuIdOpt)
        continue;
      auto itN = graph.tuNodes_.find(*tuIdOpt);
      if (itN == graph.tuNodes_.end())
        continue;
      for (CallGraph::SId usrId : itN->second) {
        auto itNode = graph.nodes_.find(usrId);
        if (itNode != graph.nodes_.end()) {
          const std::string &u = itNode->second.usr.empty()
                                     ? itNode->second.qualifiedName
                                     : itNode->second.usr;
          usrOwnerTu.emplace(u, i);
        }
      }
    }
  }

  if (firstHealthy < nTus) {
    if (auto *ents = full.getArray("entities")) {
      for (auto &val : *ents) {
        auto *obj = val.getAsObject();
        if (!obj)
          continue;
        size_t owner = firstHealthy;
        std::string f = obj->getString("file").value_or("").str();
        std::string u = obj->getString("usr").value_or("").str();
        if (auto it = tuByPath.find(f); it != tuByPath.end()) {
          owner = it->second;
        } else if (auto itU = usrOwnerTu.find(u); itU != usrOwnerTu.end()) {
          owner = itU->second;
        }
        (*obj)["module"] = meta.files[owner].path;
        tuEntities[owner].push_back(std::move(val));
      }
    }

    auto *calls = full.getArray("raw_calls");
    auto *ctxs = full.getArray("raw_contexts");
    if (calls && ctxs && calls->size() == ctxs->size()) {
      for (size_t k = 0; k < calls->size(); ++k) {
        auto *callObj = (*calls)[k].getAsObject();
        auto *ctxObj = (*ctxs)[k].getAsObject();
        if (!callObj || !ctxObj)
          continue;
        size_t owner = firstHealthy;
        std::string f = callObj->getString("file").value_or("").str();
        if (auto it = tuByPath.find(f); it != tuByPath.end()) {
          owner = it->second;
        }
        int64_t newCid = static_cast<int64_t>(tuContexts[owner].size());
        (*callObj)["module"] = meta.files[owner].path;
        (*callObj)["context_id"] = newCid;
        (*ctxObj)["context_id"] = newCid;
        tuCalls[owner].push_back(std::move((*calls)[k]));
        tuContexts[owner].push_back(std::move((*ctxs)[k]));
      }
    }

    if (auto *chs = full.getArray("channels")) {
      for (auto &val : *chs) {
        auto *obj = val.getAsObject();
        if (!obj)
          continue;
        size_t owner = firstHealthy;
        std::string f = obj->getString("file").value_or("").str();
        if (auto it = tuByPath.find(f); it != tuByPath.end()) {
          owner = it->second;
        }
        tuChannels[owner].push_back(std::move(val));
      }
    }
  }

  for (size_t i = 0; i < nTus; ++i) {
    const std::string &moduleFile = meta.files[i].path;
    std::string fpStr =
        i < meta.fingerprints.size() ? meta.fingerprints[i] : "";
    uint64_t fingerprint = parseHexOrHash(fpStr, moduleFile);
    if (fingerprint == 0)
      fingerprint = fnv1a64(moduleFile);
    const char *outcomeStr =
        i < meta.outcomes.size()
            ? mapParseOutcome(meta.outcomes[i].status)
            : "indexed";
    std::string errorDetail =
        i < meta.outcomes.size() ? meta.outcomes[i].detail : "";

    llvm::json::Object shard;
    shard["module_name"] = moduleFile;
    shard["file"] = moduleFile;
    shard["fingerprint"] = toJsonU64(fingerprint);
    shard["outcome"] = outcomeStr;
    if (errorDetail.empty())
      shard["error_detail"] = nullptr;
    else
      shard["error_detail"] = errorDetail;
    shard["entities"] = std::move(tuEntities[i]);
    shard["raw_calls"] = std::move(tuCalls[i]);
    shard["raw_contexts"] = std::move(tuContexts[i]);
    shard["imports"] = llvm::json::Array();
    shard["schemas"] = llvm::json::Array();
    shard["env_accesses"] = llvm::json::Array();
    shard["inline_suppressions"] = llvm::json::Array();
    shard["import_bindings"] = llvm::json::Object();
    shard["dunder_all"] = nullptr;
    if (i == firstHealthy) {
      if (auto *locks = full.getObject("lock_acquisitions"))
        shard["lock_acquisitions"] = std::move(*locks);
      else
        shard["lock_acquisitions"] = llvm::json::Object();
      if (auto *fnRets = full.getObject("function_returns"))
        shard["function_returns"] = std::move(*fnRets);
      else
        shard["function_returns"] = llvm::json::Object();
    } else {
      shard["lock_acquisitions"] = llvm::json::Object();
      shard["function_returns"] = llvm::json::Object();
    }
    shard["await_under_threading_lock"] = llvm::json::Array();
    shard["channels"] = std::move(tuChannels[i]);
    arr.push_back(std::move(shard));
  }
  return arr;
}

std::string MegascopeBridge::toShardJson(const CallGraph &graph,
                                         const ControlFlowIndex &cfIndex,
                                         const ChannelIndex &channels,
                                         const SnapshotMeta &meta,
                                         bool pretty) {
  llvm::json::Value val(toShardObject(graph, cfIndex, channels, meta, ""));
  std::string out;
  llvm::raw_string_ostream os(out);
  if (pretty)
    os << llvm::formatv("{0:2}\n", val);
  else
    os << val << "\n";
  return os.str();
}

std::string MegascopeBridge::toShardsJson(const CallGraph &graph,
                                          const ControlFlowIndex &cfIndex,
                                          const ChannelIndex &channels,
                                          const SnapshotMeta &meta,
                                          bool pretty) {
  llvm::json::Value val(toShardsArray(graph, cfIndex, channels, meta));
  std::string out;
  llvm::raw_string_ostream os(out);
  if (pretty)
    os << llvm::formatv("{0:2}\n", val);
  else
    os << val << "\n";
  return os.str();
}

std::string MegascopeBridge::toMetaJson(const SnapshotMeta &meta,
                                        llvm::StringRef projectRoot) {
  llvm::json::Object obj;
  obj["projectRoot"] = projectRoot.str();
  llvm::json::Array eps;
  for (const auto &ep : meta.entryPoints)
    eps.push_back(ep);
  obj["entryPoints"] = std::move(eps);

  llvm::json::Array cps;
  for (const auto &cp : meta.collapsePaths)
    cps.push_back(cp);
  obj["collapsePaths"] = std::move(cps);

  std::string out;
  llvm::raw_string_ostream os(out);
  os << llvm::json::Value(std::move(obj));
  return os.str();
}

bool MegascopeBridge::saveShardJson(const std::string &path,
                                    const CallGraph &graph,
                                    const ControlFlowIndex &cfIndex,
                                    const SnapshotMeta &meta,
                                    const ChannelIndex &channels,
                                    std::string *error, bool durable,
                                    bool pretty) {
  std::string jsonStr =
      (meta.files.size() <= 1)
          ? toShardJson(graph, cfIndex, channels, meta, pretty)
          : toShardsJson(graph, cfIndex, channels, meta, pretty);
  return writeFileAtomically(
      path, [&](llvm::raw_ostream &os) { os << jsonStr; }, error, durable);
}

bool MegascopeBridge::saveSnapshot(const std::string &path,
                                   const CallGraph &graph,
                                   const ControlFlowIndex &cfIndex,
                                   const SnapshotMeta &meta,
                                   const ChannelIndex &channels,
                                   std::string *error) {
#if defined(VYCOR_HAS_LIBMEGASCOPE)
  std::string shardsJson =
      toShardsJson(graph, cfIndex, channels, meta, /*pretty=*/false);
  std::string metaJson = toMetaJson(meta);
  char *errPtr = nullptr;
  int32_t rc = megascope_bake_shards_json(shardsJson.c_str(), metaJson.c_str(),
                                          path.c_str(), &errPtr);
  if (rc != 0) {
    if (error) {
      *error = errPtr ? std::string(errPtr)
                      : ("megascope_bake_shards_json failed with code " +
                         std::to_string(rc));
    }
    if (errPtr)
      megascope_string_free(errPtr);
    return false;
  }
  if (errPtr)
    megascope_string_free(errPtr);
  return true;
#else
  if (error)
    *error = "vycor-cpp was built without libmegascope support";
  return false;
#endif
}

bool MegascopeBridge::mergeSnapshots(
    const std::string &outputPath, const std::vector<std::string> &inputPaths,
    const std::vector<std::string> &entryPoints,
    const std::vector<std::string> &collapsePaths, std::string *error) {
#if defined(VYCOR_HAS_LIBMEGASCOPE)
  if (inputPaths.empty()) {
    if (error)
      *error = "no input snapshots specified for merge";
    return false;
  }

  std::vector<std::string> resolvedInputs;
  std::vector<std::string> tempFiles;
  auto cleanupTemps = [&]() {
    for (const auto &tf : tempFiles)
      llvm::sys::fs::remove(tf);
  };

  std::vector<std::string> mergedEntryPoints = entryPoints;
  std::vector<std::string> mergedCollapsePaths = collapsePaths;

  for (size_t i = 0; i < inputPaths.size(); ++i) {
    const std::string &inPath = inputPaths[i];
    if (isMegascopeSnapshotPath(inPath)) {
      resolvedInputs.push_back(inPath);
      continue;
    }
    if (isMegascopeShardJsonPath(inPath) ||
        llvm::StringRef(inPath).ends_with(".json")) {
      auto bufOrErr = llvm::MemoryBuffer::getFile(inPath);
      if (!bufOrErr) {
        if (error)
          *error = "failed to read shard JSON '" + inPath + "'";
        cleanupTemps();
        return false;
      }
      llvm::SmallString<256> tmpPath;
      int tmpFd = -1;
      if (llvm::sys::fs::createTemporaryFile("vycor-mgsc-shard", "mgsc", tmpFd,
                                             tmpPath)) {
        if (error)
          *error = "failed to create temporary .mgsc file";
        cleanupTemps();
        return false;
      }
      llvm::sys::fs::closeFile(tmpFd);
      std::string tmpStr = std::string(tmpPath.str());
      tempFiles.push_back(tmpStr);

      SnapshotMeta emptyMeta;
      emptyMeta.entryPoints = mergedEntryPoints;
      emptyMeta.collapsePaths = mergedCollapsePaths;
      std::string metaJson = toMetaJson(emptyMeta);
      char *errPtr = nullptr;
      int32_t rc = megascope_bake_shards_json(
          (*bufOrErr)->getBufferStart(), metaJson.c_str(), tmpStr.c_str(),
          &errPtr);
      if (rc != 0) {
        if (error)
          *error = errPtr ? std::string(errPtr)
                          : ("failed to bake shard JSON '" + inPath + "'");
        if (errPtr)
          megascope_string_free(errPtr);
        cleanupTemps();
        return false;
      }
      if (errPtr)
        megascope_string_free(errPtr);
      resolvedInputs.push_back(tmpStr);
      continue;
    }

    // Otherwise, treat as a .vycs snapshot and lower it to a temporary .mgcs
    SnapshotLoadStats loadStats;
    auto vycsOpt =
        SnapshotIO::load(inPath, &loadStats, LoadMode::Mutable, kSectionAll);
    if (!vycsOpt) {
      if (error) {
        *error = "cannot load input '" + inPath + "': " +
                 (loadStats.error.empty() ? "unrecognized snapshot format"
                                          : loadStats.error);
      }
      cleanupTemps();
      return false;
    }
    for (const auto &ep : vycsOpt->meta.entryPoints) {
      if (std::find(mergedEntryPoints.begin(), mergedEntryPoints.end(), ep) ==
          mergedEntryPoints.end()) {
        mergedEntryPoints.push_back(ep);
      }
    }
    for (const auto &cp : vycsOpt->meta.collapsePaths) {
      if (std::find(mergedCollapsePaths.begin(), mergedCollapsePaths.end(),
                    cp) == mergedCollapsePaths.end()) {
        mergedCollapsePaths.push_back(cp);
      }
    }

    llvm::SmallString<256> tmpPath;
    int tmpFd = -1;
    if (llvm::sys::fs::createTemporaryFile("vycor-vycs-converted", "mgsc",
                                           tmpFd, tmpPath)) {
      if (error)
        *error = "failed to create temporary .mgsc file";
      cleanupTemps();
      return false;
    }
    llvm::sys::fs::closeFile(tmpFd);
    std::string tmpStr = std::string(tmpPath.str());
    tempFiles.push_back(tmpStr);

    if (!saveSnapshot(tmpStr, vycsOpt->graph, vycsOpt->cfIndex, vycsOpt->meta,
                      vycsOpt->channels, error)) {
      cleanupTemps();
      return false;
    }
    resolvedInputs.push_back(tmpStr);
  }

  llvm::json::Array inputsArr;
  for (const auto &p : resolvedInputs)
    inputsArr.push_back(p);
  std::string inputsJson;
  {
    llvm::raw_string_ostream os(inputsJson);
    os << llvm::json::Value(std::move(inputsArr));
  }

  SnapshotMeta meta;
  meta.entryPoints = mergedEntryPoints;
  meta.collapsePaths = mergedCollapsePaths;
  std::string metaJson = toMetaJson(meta);

  char *errPtr = nullptr;
  int32_t rc = megascope_merge_snapshots(inputsJson.c_str(), outputPath.c_str(),
                                         metaJson.c_str(), &errPtr);
  cleanupTemps();
  if (rc != 0) {
    if (error) {
      *error = errPtr ? std::string(errPtr)
                      : ("megascope_merge_snapshots failed with code " +
                         std::to_string(rc));
    }
    if (errPtr)
      megascope_string_free(errPtr);
    return false;
  }
  if (errPtr)
    megascope_string_free(errPtr);
  return true;
#else
  if (error)
    *error = "vycor-cpp was built without libmegascope support";
  return false;
#endif
}

int MegascopeBridge::runVerbJson(llvm::StringRef verb,
                                 const llvm::json::Object &args,
                                 llvm::StringRef indexPath,
                                 llvm::StringRef shardsJson,
                                 std::string &outputOut,
                                 std::string &errorOut) {
#if defined(VYCOR_HAS_LIBMEGASCOPE)
  std::string verbStr = verb.str();
  std::string argsStr;
  {
    llvm::raw_string_ostream os(argsStr);
    os << llvm::json::Value(llvm::json::Object(args));
  }
  std::string indexPathStr = indexPath.str();
  std::string shardsJsonStr = shardsJson.str();

  char *outPtr = nullptr;
  char *errPtr = nullptr;
  int32_t rc = megascope_run_verb_json(
      verbStr.c_str(), argsStr.c_str(),
      indexPathStr.empty() ? nullptr : indexPathStr.c_str(),
      shardsJsonStr.empty() ? nullptr : shardsJsonStr.c_str(), &outPtr,
      &errPtr);
  if (outPtr) {
    outputOut = outPtr;
    megascope_string_free(outPtr);
  }
  if (errPtr) {
    errorOut = errPtr;
    megascope_string_free(errPtr);
  }
  return static_cast<int>(rc);
#else
  errorOut = "vycor-cpp was built without libmegascope support";
  return 3;
#endif
}

} // namespace vycor
