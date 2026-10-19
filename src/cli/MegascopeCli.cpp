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

#include "vycor/cli/MegascopeCli.h"
#include "vycor/callgraph/AtomicFile.h"
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/MegascopeBridge.h"
#include "vycor/callgraph/RetainUntilExit.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/callgraph/Utf8.h"
#include "vycor/cli/BakeConfig.h"
#include "vycor/cli/SourceSelection.h"
#include "vycor/query/Serialize.h"

#include "clang/Tooling/CompilationDatabase.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace vycor {

// ============================================================================
// Names and paths
// ============================================================================

std::string defaultIndexPath(llvm::StringRef buildPath) {
  llvm::StringRef baseDir =
      buildPath.empty() ? llvm::StringRef(".") : buildPath;
  if (!buildPath.empty() && llvm::sys::fs::is_regular_file(buildPath)) {
    baseDir = llvm::sys::path::parent_path(buildPath);
    if (baseDir.empty())
      baseDir = ".";
  }
  llvm::SmallString<256> p(baseDir);
  llvm::sys::path::append(p, ".vycor", "megascope.vycs");
  return p.str().str();
}

std::string resolveIndexPath(llvm::StringRef explicitIndex,
                             llvm::StringRef envIndex,
                             llvm::StringRef buildPath) {
  if (!explicitIndex.empty())
    return explicitIndex.str();
  if (!envIndex.empty())
    return envIndex.str();
  return defaultIndexPath(buildPath);
}

std::string canonicalToolName(llvm::StringRef verb) {
  std::string name = verb.str();
  std::replace(name.begin(), name.end(), '-', '_');
  return name;
}

/// Command-line spelling of a registered tool name.
static std::string hyphenated(llvm::StringRef name) {
  std::string out = name.str();
  std::replace(out.begin(), out.end(), '_', '-');
  return out;
}

bool isMegascopeQueryVerb(llvm::StringRef verb) {
  return !verb.empty() && !verb.starts_with("-") && verb != "index";
}

// ============================================================================
// Schema -> flags
// ============================================================================

namespace {

struct FlagSpec {
  std::string name; // schema property name (underscores)
  std::string type; // string | integer | boolean | array
  std::string itemType;
  std::string description;
  bool required = false;
};

std::vector<FlagSpec> schemaFlags(const ToolEntry &tool) {
  std::vector<FlagSpec> out;
  const auto *schema = tool.inputSchema.getAsObject();
  if (!schema)
    return out;
  std::set<std::string> required;
  if (const auto *req = schema->getArray("required"))
    for (const auto &r : *req)
      if (auto s = r.getAsString())
        required.insert(s->str());
  const auto *props = schema->getObject("properties");
  if (!props)
    return out;
  for (const auto &kv : *props) {
    FlagSpec f;
    f.name = kv.first.str();
    f.type = "string";
    if (const auto *p = kv.second.getAsObject()) {
      f.type = p->getString("type").value_or("string").str();
      f.description = p->getString("description").value_or("").str();
      if (f.type == "array") {
        f.itemType = "string";
        if (const auto *items = p->getObject("items"))
          f.itemType = items->getString("type").value_or("string").str();
      }
    }
    f.required = required.count(f.name) > 0;
    out.push_back(std::move(f));
  }
  std::sort(out.begin(), out.end(), [](const FlagSpec &a, const FlagSpec &b) {
    return a.name < b.name;
  });
  return out;
}

std::string flagList(const std::vector<FlagSpec> &specs) {
  std::string s;
  for (const auto &f : specs) {
    if (!s.empty())
      s += ", ";
    s += "--" + hyphenated(f.name);
  }
  return s;
}

llvm::Error usageError(const llvm::Twine &msg) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), msg);
}

/// Word-wrap `text` at `width`, prefixing every line with `indent`.
void wrapText(llvm::StringRef text, unsigned width, llvm::StringRef indent,
              llvm::raw_ostream &os) {
  unsigned col = 0;
  os << indent;
  llvm::SmallVector<llvm::StringRef, 64> words;
  text.split(words, ' ', -1, /*KeepEmpty=*/false);
  for (llvm::StringRef w : words) {
    if (col && col + 1 + w.size() > width) {
      os << "\n" << indent;
      col = 0;
    }
    if (col) {
      os << ' ';
      ++col;
    }
    os << w;
    col += w.size();
  }
  os << "\n";
}

} // namespace

llvm::Expected<llvm::json::Object>
parseToolArgs(const ToolEntry &tool, llvm::ArrayRef<std::string> argv,
              llvm::json::Object seed) {
  const auto specs = schemaFlags(tool);
  auto fail = [&](const llvm::Twine &msg) {
    return usageError(msg + " (valid flags for " + hyphenated(tool.name) +
                      ": " + flagList(specs) + ")");
  };
  llvm::json::Object args = std::move(seed);
  // Arrays given on the command line replace the seeded value on their
  // first occurrence and append on later ones.
  std::set<std::string> arraysSeen;

  for (size_t i = 0; i < argv.size(); ++i) {
    llvm::StringRef a = argv[i];
    if (!a.starts_with("--") || a.size() == 2)
      return fail("unexpected argument '" + a + "'");
    llvm::StringRef body = a.drop_front(2);
    const bool hasInline = body.contains('=');
    const std::pair<llvm::StringRef, llvm::StringRef> kv = body.split('=');
    const llvm::StringRef rawKey = kv.first, inlineVal = kv.second;
    const std::string key = canonicalToolName(rawKey);
    const auto spec = std::find_if(
        specs.begin(), specs.end(),
        [&](const FlagSpec &f) { return f.name == key; });
    if (spec == specs.end())
      return fail("unknown flag '--" + rawKey + "'");

    auto takeValue = [&]() -> llvm::Expected<llvm::StringRef> {
      if (hasInline)
        return inlineVal;
      if (i + 1 >= argv.size() ||
          llvm::StringRef(argv[i + 1]).starts_with("--"))
        return fail("flag '--" + rawKey + "' requires a value");
      return llvm::StringRef(argv[++i]);
    };

    if (spec->type == "boolean") {
      bool value = true;
      if (hasInline) {
        if (inlineVal == "true" || inlineVal == "1")
          value = true;
        else if (inlineVal == "false" || inlineVal == "0")
          value = false;
        else
          return fail("flag '--" + rawKey + "' expects true or false");
      }
      args[key] = value;
      continue;
    }

    auto value = takeValue();
    if (!value)
      return value.takeError();

    if (spec->type == "integer") {
      int64_t n = 0;
      if (value->getAsInteger(10, n))
        return fail("flag '--" + rawKey + "' expects an integer, got '" +
                    *value + "'");
      args[key] = n;
    } else if (spec->type == "array") {
      // Command-line bytes need not be UTF-8: a raw spelling becomes the
      // index text it is stored as (Utf8.h lookupText).
      llvm::json::Value item(lookupText(*value));
      if (spec->itemType == "integer") {
        int64_t n = 0;
        if (value->getAsInteger(10, n))
          return fail("flag '--" + rawKey + "' expects an integer, got '" +
                      *value + "'");
        item = n;
      }
      const bool firstUse = arraysSeen.insert(key).second;
      llvm::json::Array *arr = args.getArray(key);
      if (firstUse || !arr) {
        args[key] = llvm::json::Array{};
        arr = args.getArray(key);
      }
      arr->push_back(std::move(item));
    } else {
      args[key] = lookupText(*value);
    }
  }

  for (const auto &f : specs)
    if (f.required && !args.get(f.name))
      return fail("missing required flag '--" + hyphenated(f.name) + "'");
  return args;
}

// Worked examples per tool for --help. Agents read --help, not docs/
// (docs/megascope-cli-review.md §4.3); tools absent here get none.
static const std::unordered_map<std::string, std::vector<const char *>>
    kToolExamples = {
        {"search_functions",
         {"megascope search-functions --query Replicator --limit 20",
          "megascope search-functions --query Replicator --format tsv | "
          "cut -f2-"}},
        {"lookup_function", {"megascope lookup-function --name Foo::bar"}},
        {"get_callers",
         {"megascope get-callers --name Foo::bar",
          "megascope get-callers --name Foo::bar --format ndjson | "
          "jq -r 'select(.callerName) | .callerName' | sort -u"}},
        {"get_callees",
         {"megascope get-callees --name Foo::bar --format tsv",
          "megascope get-callees --usr 'c:@N@ns@F@f#' --min-confidence "
          "Proven"}},
        {"find_call_chain",
         {"megascope find-call-chain --from main --to Foo::bar --max-depth 8",
          "megascope find-call-chain --from main --to Foo::bar --format "
          "ndjson | jq -r 'select(type==\"array\") | [.[0].from] + map(.to) "
          "| join(\" -> \")'"}},
        {"query_exception_safety",
         {"megascope query-exception-safety --function Foo::bar "
          "--exception-type std::exception --entry-point main"}},
        {"query_call_site_context",
         {"megascope query-call-site-context --call-site "
          "/abs/src/foo.cpp:12:3"}},
        {"query_throw_propagation",
         {"megascope query-throw-propagation --function Foo::bar "
          "--exception-type std::runtime_error --format ndjson"}},
        {"query_all_path_contexts",
         {"megascope query-all-path-contexts --function Foo::bar "
          "--max-paths 10 --format ndjson"}},
        {"query_nearest_catches",
         {"megascope query-nearest-catches --function Foo::bar"}},
        {"analyze_dead_code",
         {"megascope analyze-dead-code --format ndjson"}},
        {"graph_summary", {"megascope graph-summary --pretty"}},
        {"list_entry_points",
         {"megascope list-entry-points --format tsv"}},
        {"get_class_hierarchy",
         {"megascope get-class-hierarchy --class-name Base"}},
    };

void printToolHelp(const ToolEntry &tool, llvm::raw_ostream &os) {
  os << "Usage: vycor-cpp megascope " << hyphenated(tool.name)
     << " [--index <file>] [flags]\n\n";
  wrapText(tool.description, 78, "", os);
  const auto specs = schemaFlags(tool);
  if (!specs.empty())
    os << "\nFlags (from the tool's input schema):\n";
  for (const auto &f : specs) {
    std::string head = "  --" + hyphenated(f.name);
    if (f.type == "integer")
      head += " <int>";
    else if (f.type == "array")
      head += " <" + f.itemType + ">  (repeatable)";
    else if (f.type != "boolean")
      head += " <" + f.type + ">";
    if (f.required)
      head += "  [required]";
    os << head << "\n";
    if (!f.description.empty())
      wrapText(f.description, 78, "      ", os);
  }
  auto ex = kToolExamples.find(tool.name);
  if (ex != kToolExamples.end()) {
    os << "\nExamples:\n";
    for (const char *line : ex->second)
      os << "  vycor-cpp " << line << "\n";
  }
  os << "\nCommon flags:\n"
        "  --index <file>        Index file. Default: $VYCOR_INDEX, then\n"
        "                        <build-path>/.vycor/megascope.vycs, then\n"
        "                        ./.vycor/megascope.vycs\n"
        "  --build-path <dir>    Build directory the default index path is\n"
        "                        derived from; with --source, where its\n"
        "                        compile_commands.json lives\n"
        "  --source <file>       Bake this TU in memory and answer from that\n"
        "                        instead of reading an index (repeatable;\n"
        "                        also --source-list, --source-re, and the\n"
        "                        index verb's --skip-paths, --collapse-paths,\n"
        "                        --threads, --org-config)\n"
        "  --entry-point <name>  Default entry point set for tools that take\n"
        "                        one (repeatable; default: main)\n"
        "  --args <json>         JSON object of arguments; explicit flags\n"
        "                        override its members\n"
        "  --format <fmt>        json (default) | ndjson | tsv\n"
        "  --pretty              Indent JSON output\n"
        "  -v, --verbose         Progress on stderr\n"
        "\nEvery payload carries `status` (ok, ambiguous, usage_error,\n"
        "not_found, unavailable) and `indexScope` (bake, freshness, TU\n"
        "coverage); see docs/result-contract.md.\n"
        "\nExit codes: 0 results, 1 empty or not found, 2 usage, 3 index\n"
        "missing, unreadable, or lacking the needed facts, 4 ambiguous\n"
        "identity (candidates on stdout).\n";
}

// ============================================================================
// Output contract
// ============================================================================

namespace {

llvm::StringRef effectiveRecordsKey(const llvm::json::Value &payload,
                                    llvm::StringRef recordsKey) {
  return isAmbiguousResult(payload) ? llvm::StringRef("candidates")
                                    : recordsKey;
}

const llvm::json::Array *recordsOf(const llvm::json::Value &payload,
                                   llvm::StringRef recordsKey) {
  const auto *obj = payload.getAsObject();
  if (!obj)
    return nullptr;
  llvm::StringRef key = effectiveRecordsKey(payload, recordsKey);
  return key.empty() ? nullptr : obj->getArray(key);
}

void writeJson(llvm::raw_ostream &os, const llvm::json::Value &v,
               bool pretty) {
  if (pretty)
    os << llvm::formatv("{0:2}", v);
  else
    os << v;
  os << "\n";
}

std::string tsvEscape(llvm::StringRef s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
    case '\t':
      out += "\\t";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\\':
      out += "\\\\";
      break;
    default:
      out += c;
    }
  }
  return out;
}

void writeTsvCell(llvm::raw_ostream &os, const llvm::json::Value *v) {
  if (!v || v->kind() == llvm::json::Value::Null)
    return;
  if (auto s = v->getAsString()) {
    os << tsvEscape(*s);
    return;
  }
  os << *v; // numbers, booleans, and nested values as compact JSON
}

void writeTsv(llvm::raw_ostream &os, const llvm::json::Value &payload,
              llvm::StringRef recordsKey) {
  // Rows: the records list, or the payload itself as one row.
  std::vector<const llvm::json::Value *> rows;
  if (const auto *records = recordsOf(payload, recordsKey)) {
    if (records->empty())
      return; // no rows, no header: the columns are unknown
    for (const auto &r : *records)
      rows.push_back(&r);
  } else {
    rows.push_back(&payload);
  }
  // Columns: sorted union of object keys (deterministic regardless of
  // which fields a given record happens to carry).
  std::set<std::string> columns;
  bool anyObject = false;
  for (const auto *r : rows)
    if (const auto *obj = r->getAsObject()) {
      anyObject = true;
      for (const auto &kv : *obj)
        columns.insert(kv.first.str());
    }
  if (!anyObject) {
    os << "value\n";
    for (const auto *r : rows) {
      writeTsvCell(os, r);
      os << "\n";
    }
    return;
  }
  bool first = true;
  for (const auto &c : columns) {
    os << (first ? "" : "\t") << c;
    first = false;
  }
  os << "\n";
  for (const auto *r : rows) {
    const auto *obj = r->getAsObject();
    first = true;
    for (const auto &c : columns) {
      os << (first ? "" : "\t");
      first = false;
      writeTsvCell(os, obj ? obj->get(c) : nullptr);
    }
    os << "\n";
  }
}

void writeNdjson(llvm::raw_ostream &os, const llvm::json::Value &payload,
                 llvm::StringRef recordsKey) {
  const auto *records = recordsOf(payload, recordsKey);
  if (!records) {
    writeJson(os, payload, false);
    return;
  }
  llvm::StringRef key = effectiveRecordsKey(payload, recordsKey);
  llvm::json::Object summary;
  for (const auto &kv : *payload.getAsObject())
    if (llvm::StringRef(kv.first) != key)
      summary[kv.first] = kv.second;
  llvm::json::Object head;
  head["_summary"] = std::move(summary);
  writeJson(os, llvm::json::Value(std::move(head)), false);
  for (const auto &r : *records)
    writeJson(os, r, false);
}

} // namespace

int exitCodeFor(const llvm::json::Value &payload, llvm::StringRef recordsKey) {
  switch (statusOf(payload)) {
  case ResultStatus::UsageError:
    return kExitUsage;
  case ResultStatus::NotFound:
    return kExitEmpty;
  case ResultStatus::Unavailable:
    return kExitIndex;
  case ResultStatus::Ambiguous:
    return kExitAmbiguous;
  case ResultStatus::Ok:
    break;
  }
  if (const auto *records = recordsOf(payload, recordsKey))
    return records->empty() ? kExitEmpty : kExitResults;
  return kExitResults;
}

int emitToolResult(const llvm::json::Value &payload, llvm::StringRef recordsKey,
                   OutputFormat format, bool pretty, llvm::raw_ostream &out,
                   llvm::raw_ostream &err, llvm::StringRef tool) {
  const int code = exitCodeFor(payload, recordsKey);
  if (auto msg = errorMessage(payload)) {
    if (!tool.empty())
      err << "megascope " << tool << ": " << *msg << "\n";
    if (format == OutputFormat::Tsv)
      out << "error\tstatus\n" << tsvEscape(*msg) << "\t"
          << resultStatusName(statusOf(payload)) << "\n";
    else
      writeJson(out, payload, pretty && format == OutputFormat::Json);
    return code;
  }
  switch (format) {
  case OutputFormat::Json:
    writeJson(out, payload, pretty);
    break;
  case OutputFormat::Ndjson:
    writeNdjson(out, payload, recordsKey);
    break;
  case OutputFormat::Tsv:
    writeTsv(out, payload, recordsKey);
    break;
  }
  return code;
}

// ============================================================================
// Verb runner
// ============================================================================

namespace {

struct CommonOpts {
  std::string index;
  std::string buildPath;
  std::string argsJson;
  std::string format; // empty = the verb's default
  bool pretty = false;
  bool verbose = false;
  bool help = false;
  bool files = false; // info --files
  std::vector<std::string> entryPoints;
  // Ephemeral mode (§2.1): any of the three selection flags means "bake
  // these TUs in memory and answer" instead of reading an index. The
  // remaining four only qualify that bake.
  std::vector<std::string> sources;   // --source
  std::string sourceList;             // --source-list
  std::string sourceRe;               // --source-re
  std::vector<std::string> skipPaths; // --skip-paths
  std::vector<std::string> collapsePaths;
  std::string threads;
  std::string orgConfig;
  bool hasFixedCompileArgs = false;          // whether `--` was given
  std::vector<std::string> fixedCompileArgs; // after `--`
  std::vector<std::string> rest;             // everything else, in order

  bool ephemeral() const {
    return !sources.empty() || !sourceList.empty() || !sourceRe.empty();
  }
};

/// Split the common query flags out of `argv`. The tool flags that remain
/// go to `rest` untouched, so a tool property can never be shadowed by
/// accident: only the names below are claimed.
llvm::Expected<CommonOpts> splitCommonFlags(llvm::ArrayRef<std::string> argv,
                                            bool acceptFiles) {
  CommonOpts o;
  for (size_t i = 0; i < argv.size(); ++i) {
    llvm::StringRef a = argv[i];
    if (a == "--") {
      o.hasFixedCompileArgs = true;
      for (size_t j = i + 1; j < argv.size(); ++j)
        o.fixedCompileArgs.push_back(argv[j]);
      break;
    }
    if (a == "-h" || a == "--help") {
      o.help = true;
      continue;
    }
    if (a == "-v" || a == "--verbose") {
      o.verbose = true;
      continue;
    }
    if (a == "--pretty") {
      o.pretty = true;
      continue;
    }
    if (acceptFiles && a == "--files") {
      o.files = true;
      continue;
    }
    llvm::StringRef body = a.starts_with("--") ? a.drop_front(2) : a;
    const bool hasInline = a.starts_with("--") && body.contains('=');
    auto [rawKey, inlineVal] = body.split('=');
    // Same spelling rule as the tool flags: underscores read as hyphens.
    std::string keyStorage = rawKey.str();
    std::replace(keyStorage.begin(), keyStorage.end(), '_', '-');
    const llvm::StringRef key = keyStorage;
    std::string *target = nullptr;
    std::vector<std::string> *listTarget = nullptr;
    if (a.starts_with("--")) {
      if (key == "index")
        target = &o.index;
      else if (key == "build-path")
        target = &o.buildPath;
      else if (key == "args")
        target = &o.argsJson;
      else if (key == "format")
        target = &o.format;
      else if (key == "source-list")
        target = &o.sourceList;
      else if (key == "source-re")
        target = &o.sourceRe;
      else if (key == "threads")
        target = &o.threads;
      else if (key == "org-config")
        target = &o.orgConfig;
      else if (key == "entry-point")
        listTarget = &o.entryPoints;
      else if (key == "source")
        listTarget = &o.sources;
      else if (key == "skip-paths")
        listTarget = &o.skipPaths;
      else if (key == "collapse-paths")
        listTarget = &o.collapsePaths;
    }
    if (!target && !listTarget) {
      o.rest.push_back(argv[i]);
      continue;
    }
    llvm::StringRef value;
    if (hasInline) {
      value = inlineVal;
    } else if (i + 1 < argv.size() &&
               !llvm::StringRef(argv[i + 1]).starts_with("--")) {
      value = argv[++i];
    } else {
      return usageError("flag '--" + key + "' requires a value");
    }
    if (listTarget)
      listTarget->push_back(value.str());
    else
      *target = value.str();
  }
  return o;
}

llvm::Expected<OutputFormat> parseFormat(llvm::StringRef s) {
  if (s == "json")
    return OutputFormat::Json;
  if (s == "ndjson")
    return OutputFormat::Ndjson;
  if (s == "tsv")
    return OutputFormat::Tsv;
  return usageError("unknown --format '" + s +
                    "' (expected json, ndjson, or tsv)");
}

void printVerbHelp(llvm::raw_ostream &os) {
  os << "Usage: vycor-cpp megascope <verb> [flags]\n"
        "\n"
        "Verbs:\n"
        "  index     Bake the call graph and control-flow index for a\n"
        "            compilation database (or compile flags after `--`)\n"
        "            and save it (--build-path;\n"
        "            --source/--source-list/--source-re/--skip-paths\n"
        "            select TUs, default: the TUs already in the index,\n"
        "            else the whole database; `megascope index --help`)\n"
        "  merge     Combine per-TU or per-target .vycs shards into one\n"
        "            index: --output OUT [IN.vycs ...] [--input-list F]\n"
        "  <tool>    Run one query tool against a saved index, e.g.\n"
        "            `megascope get-callers --name Foo::bar`\n"
        "  call      Run a tool from a JSON argument object:\n"
        "            `megascope call get_callers --args '{\"name\":\"f\"}'`\n"
        "  tools     List the query tools (--format json for schemas)\n"
        "  info      Describe a saved index (--files lists indexed TUs)\n"
        "  batch     Answer NDJSON requests {\"tool\":..,\"args\":{..}} from\n"
        "            stdin, one JSON response per line, on one loaded index\n"
        "  dump      Stream every call-site context and channel site\n"
        "            (--format ndjson, the default, or json)\n"
        "  diff      Compare two saved indexes: --before A --after B\n"
        "            [--to NAME] [--impact] (docs/change-impact.md)\n"
        "\n"
        "Query verbs read the index from --index, $VYCOR_INDEX,\n"
        "<build-path>/.vycor/megascope.vycs, or ./.vycor/megascope.vycs.\n"
        "Given --source/--source-list/--source-re (with --build-path or\n"
        "`-- <compile-flags...>`) they bake the selected TUs in memory\n"
        "instead and answer from that; no index is read or written\n"
        "(--skip-paths, --collapse-paths, --threads, and --org-config\n"
        "qualify the bake as for `index`).\n"
        "Run `megascope <tool> --help` for a tool's flags.\n"
        "\n"
        "Every payload carries `status` (ok, ambiguous, usage_error,\n"
        "not_found, unavailable) and `indexScope` (bake, freshness, TU\n"
        "coverage); see docs/result-contract.md.\n"
        "\n"
        "Exit codes: 0 results, 1 empty or not found, 2 usage, 3 index\n"
        "missing, unreadable, or lacking the needed facts, 4 ambiguous\n"
        "identity (candidates on stdout).\n";
}

/// Up to the first sentence-ending ". " — skipping the abbreviations the
/// tool descriptions use ("e.g. ", "i.e. ", "vs. ").
llvm::StringRef firstSentence(llvm::StringRef text) {
  size_t dot = text.find(". ");
  while (dot != llvm::StringRef::npos) {
    llvm::StringRef before = text.substr(0, dot);
    if (!before.ends_with("e.g") && !before.ends_with("i.e") &&
        !before.ends_with("vs"))
      return text.substr(0, dot + 1);
    dot = text.find(". ", dot + 1);
  }
  return text;
}

int runTools(const std::vector<ToolEntry> &tools, const CommonOpts &common,
             llvm::raw_ostream &out, llvm::raw_ostream &err) {
  if (!common.rest.empty()) {
    err << "megascope tools: unexpected argument '" << common.rest.front()
        << "'\n";
    return kExitUsage;
  }
  const char *stray =
      !common.index.empty()           ? "--index"
      : !common.buildPath.empty()     ? "--build-path"
      : !common.argsJson.empty()      ? "--args"
      : !common.entryPoints.empty()   ? "--entry-point"
      : common.ephemeral()            ? "--source/--source-list/--source-re"
      : !common.skipPaths.empty()     ? "--skip-paths"
      : !common.collapsePaths.empty() ? "--collapse-paths"
      : !common.threads.empty()       ? "--threads"
      : !common.orgConfig.empty()     ? "--org-config"
      : common.hasFixedCompileArgs    ? "-- <compile-flags>"
                                      : nullptr;
  if (stray) {
    err << "megascope tools: " << stray
        << " does not apply to `megascope tools`\n";
    return kExitUsage;
  }
  if (common.format.empty()) {
    size_t width = 0;
    for (const auto &t : tools)
      width = std::max(width, hyphenated(t.name).size());
    for (const auto &t : tools) {
      std::string verb = hyphenated(t.name);
      out << "  " << verb << std::string(width + 2 - verb.size(), ' ')
          << firstSentence(t.description) << "\n";
    }
    return kExitResults;
  }
  auto format = parseFormat(common.format);
  if (!format) {
    err << "megascope tools: " << llvm::toString(format.takeError()) << "\n";
    return kExitUsage;
  }
  llvm::json::Array arr;
  for (const auto &t : tools) {
    llvm::json::Object o;
    o["name"] = t.name;
    o["verb"] = hyphenated(t.name);
    o["description"] = t.description;
    o["inputSchema"] = t.inputSchema;
    o["records"] = t.recordsKey;
    o["needs"] = llvm::json::Array(sectionNames(t.needs));
    arr.push_back(llvm::json::Value(std::move(o)));
  }
  llvm::json::Object payload;
  payload["count"] = static_cast<int64_t>(arr.size());
  payload["tools"] = std::move(arr);
  return emitToolResult(llvm::json::Value(std::move(payload)), "tools",
                        *format, common.pretty, out, err);
}

int runInfo(const SnapshotData &snap, llvm::StringRef indexPath,
            const CommonOpts &common, OutputFormat format,
            llvm::raw_ostream &out, llvm::raw_ostream &err) {
  llvm::json::Object o;
  o["index"] = toIndexText(indexPath);
  o["format_version"] = static_cast<int64_t>(SnapshotIO::kFormatVersion);
  uint64_t bytes = 0;
  if (!llvm::sys::fs::file_size(indexPath, bytes))
    o["index_bytes"] = static_cast<int64_t>(bytes);
  o["file_count"] = static_cast<int64_t>(snap.meta.files.size());
  o["dependency_count"] = static_cast<int64_t>(snap.meta.deps.size());
  // From the header: info decodes the meta section only.
  o["nodes"] = static_cast<int64_t>(snap.summary.nodes);
  o["edges"] = static_cast<int64_t>(snap.summary.edges);
  o["call_sites"] = static_cast<int64_t>(snap.summary.callSites);
  o["channel_sites"] = static_cast<int64_t>(snap.summary.channelSites);
  // The meta keeps its strings as written (Utf8.h): converted here.
  auto texts = [](const std::vector<std::string> &v) {
    llvm::json::Array a;
    for (const auto &s : v)
      a.push_back(toIndexText(s));
    return a;
  };
  o["entry_points"] = texts(snap.meta.entryPoints);

  llvm::json::Object cfg;
  cfg["collapse_paths"] = texts(snap.meta.collapsePaths);
  cfg["lock_types"] = texts(snap.meta.lockAllowlist);
  cfg["lock_builtins"] = snap.meta.lockBuiltins;
  llvm::json::Array channelTypes;
  for (const auto &ct : snap.meta.channelTypes) {
    llvm::json::Object c;
    c["type"] = toIndexText(ct.qualifiedTypeName);
    c["category"] = toIndexText(ct.category);
    c["produce"] = texts(ct.produceMethods);
    c["consume"] = texts(ct.consumeMethods);
    channelTypes.push_back(llvm::json::Value(std::move(c)));
  }
  cfg["channel_types"] = std::move(channelTypes);
  o["config"] = std::move(cfg);

  // Provenance and coverage (docs/index-provenance.md): how the index was
  // produced and how much of the requested scope it holds. Both come from
  // the meta section alone.
  llvm::json::Object prov;
  prov["analyzer"] = toIndexText(snap.meta.provenance.analyzer);
  prov["toolchain"] = toIndexText(snap.meta.provenance.toolchain);
  prov["environment"] = toIndexText(snap.meta.provenance.environment);
  prov["bake_start_ns"] =
      static_cast<int64_t>(snap.meta.provenance.bakeStartNs);
  // The reference a tool payload's indexScope.bake carries.
  const IndexFacts facts =
      IndexFacts::of(snap.meta, IndexFreshness::Unchecked);
  if (!facts.bake.empty())
    prov["bake"] = facts.bake; // already index text
  o["provenance"] = std::move(prov);
  // info never compares the index with the sources.
  o["freshness"] = "unchecked";
  const IndexCoverage cov = coverageOf(snap.meta);
  llvm::json::Object coverage;
  coverage["requested"] = static_cast<int64_t>(cov.requested);
  coverage["indexed"] = static_cast<int64_t>(cov.indexed);
  coverage["partial"] = static_cast<int64_t>(cov.partial);
  coverage["failed"] = static_cast<int64_t>(cov.failed);
  coverage["complete"] = cov.complete();
  o["coverage"] = std::move(coverage);

  if (common.files) {
    llvm::json::Array files;
    for (size_t i = 0; i < snap.meta.files.size(); ++i) {
      const auto &fs = snap.meta.files[i];
      llvm::json::Object f;
      f["path"] = toIndexText(fs.path);
      f["mtime_ns"] = static_cast<int64_t>(fs.mtimeNs);
      f["size"] = static_cast<int64_t>(fs.size);
      const TuOutcome outcome = i < snap.meta.outcomes.size()
                                    ? snap.meta.outcomes[i]
                                    : TuOutcome{};
      f["status"] = tuStatusName(outcome.status);
      if (!outcome.detail.empty())
        f["detail"] = toIndexText(outcome.detail);
      f["fingerprint"] = i < snap.meta.fingerprints.size()
                             ? toIndexText(snap.meta.fingerprints[i])
                             : std::string();
      files.push_back(llvm::json::Value(std::move(f)));
    }
    o["files"] = std::move(files);
  }
  // "files" only lays out ndjson/tsv: an index with no TUs is still a
  // fully answered description, so --files must not flip the exit code.
  int code = emitToolResult(llvm::json::Value(std::move(o)),
                            common.files ? "files" : "", format,
                            common.pretty, out, err);
  return code == kExitEmpty ? kExitResults : code;
}

} // namespace

int runBatch(const std::vector<ToolEntry> &tools, const ToolContext &ctx,
             std::istream &in, llvm::raw_ostream &out) {
  std::unordered_map<std::string, const ToolEntry *> byName;
  for (const auto &t : tools)
    byName[t.name] = &t;

  std::string line;
  size_t lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    if (llvm::StringRef(line).trim().empty())
      continue;
    llvm::json::Object resp;
    auto emit = [&]() {
      out << llvm::json::Value(std::move(resp)) << "\n";
      out.flush(); // a pipe reader sees each answer as it is produced
    };
    auto batchError = [&](const llvm::Twine &msg) {
      resp["error"] = ("line " + llvm::Twine(lineNo) + ": " + msg).str();
      resp["status"] = resultStatusName(ResultStatus::UsageError);
      resp["exit"] = static_cast<int64_t>(kExitUsage);
      emit();
    };

    auto parsed = llvm::json::parse(line);
    if (!parsed) {
      batchError(llvm::toString(parsed.takeError()));
      continue;
    }
    const auto *req = parsed->getAsObject();
    if (!req) {
      batchError("request must be a JSON object");
      continue;
    }
    if (const auto *id = req->get("id"))
      resp["id"] = *id;
    auto toolName = req->getString("tool");
    if (!toolName) {
      batchError("missing 'tool'");
      continue;
    }
    const std::string canon = canonicalToolName(*toolName);
    resp["tool"] = canon;
    auto it = byName.find(canon);
    if (it == byName.end()) {
      batchError("unknown tool '" + *toolName + "'");
      continue;
    }
    llvm::json::Object args;
    if (const auto *a = req->getObject("args"))
      args = *a;
    else if (const auto *a = req->getObject("arguments"))
      args = *a;
    llvm::json::Value payload = runTool(*it->second, args, ctx);
    resp["status"] = resultStatusName(statusOf(payload));
    resp["exit"] = static_cast<int64_t>(
        exitCodeFor(payload, it->second->recordsKey));
    resp["result"] = std::move(payload);
    emit();
  }
  return kExitResults;
}

// ============================================================================
// Ephemeral mode (§2.1)
// ============================================================================

/// Bake the TUs `common` selects in memory — the query-verb counterpart of
/// `megascope index`, minus the file: the same selection flags, the same
/// single-parse bake, no snapshot read or written. What `prism --mode
/// query` used to do. Returns kExitResults with `snapOut` set (leaked on
/// purpose, like a loaded index), else the exit code to return.
static int bakeEphemeral(const CommonOpts &common, llvm::StringRef verb,
                         std::istream &in, llvm::raw_ostream &err,
                         SnapshotData *&snapOut) {
  const std::string prefix = ("megascope " + verb + ": ").str();
  if (!common.index.empty()) {
    err << prefix
        << "--source/--source-list/--source-re bake the selected TUs in "
           "memory; drop --index, or run `megascope index` to bake to a "
           "file\n";
    return kExitUsage;
  }
  if (common.buildPath.empty() && !common.hasFixedCompileArgs) {
    err << prefix << "--build-path is required with --source/--source-list/"
                     "--source-re (or pass compile flags after '--')\n";
    return kExitUsage;
  }
  if (verb == "info") {
    err << prefix << "describes a saved index; `megascope index` writes one\n";
    return kExitUsage;
  }
  if (verb == "batch" && common.sourceList == "-") {
    err << prefix << "stdin carries the requests; --source-list - cannot "
                     "read it too\n";
    return kExitUsage;
  }
  unsigned threads = 0;
  if (!common.threads.empty() &&
      llvm::StringRef(common.threads).getAsInteger(10, threads)) {
    err << prefix << "--threads expects a number, got '" << common.threads
        << "'\n";
    return kExitUsage;
  }

  std::string dbError;
  auto compDb = loadCompilationDatabase(common.buildPath,
                                        common.fixedCompileArgs, dbError,
                                        common.hasFixedCompileArgs);
  if (!compDb) {
    err << "megascope: cannot load compilation database from "
        << (common.buildPath.empty() ? "<command-line>" : common.buildPath)
        << ": " << dbError << "\n";
    return kExitIndex;
  }
  SourceSelection sel;
  sel.explicitFiles = common.sources;
  sel.listFile = common.sourceList;
  sel.regex = common.sourceRe;
  sel.skipPaths = common.skipPaths;
  auto files = selectSources(*compDb, sel, in);
  if (!files) {
    err << "megascope: " << llvm::toString(files.takeError()) << "\n";
    return kExitUsage;
  }
  if (files->empty()) {
    err << "megascope: the selection matches no TU of "
        << (common.buildPath.empty() ? "<command-line>" : common.buildPath)
        << "\n";
    return kExitIndex;
  }

  std::vector<std::string> collapsePaths = common.collapsePaths;
  LockTypeConfig lockCfg;
  ChannelTypeConfig channelCfg;
  OrgConfig orgCfg;
  if (!loadOrgConfigIfSet(common.orgConfig, orgCfg))
    return kExitUsage;
  mergeExtensionConfig(orgCfg, lockCfg, channelCfg, collapsePaths);

  const auto t0 = std::chrono::steady_clock::now();
  auto baked = bakeIndexes(*compDb, *files, collapsePaths, threads, nullptr,
                           "", lockCfg, nullptr, nullptr, channelCfg);
  auto *snap = retainUntilExit(new SnapshotData);
  snap->graph = std::move(baked.graph);
  snap->cfIndex = std::move(baked.cfIndex);
  snap->channels = std::move(baked.channels);
  snap->meta.collapsePaths = collapsePaths;
  snap->meta.channelTypes = channelCfg.registeredTypes;
  // The in-memory index carries the same coverage facts a saved one
  // would, and an incomplete bake is said out loud: a query over it
  // cannot tell that a TU's facts are missing.
  snap->meta.files = SnapshotIO::stampFiles(*files);
  SnapshotIO::recordOutcomes(snap->meta, baked.outcomes);
  const IndexCoverage cov = coverageOf(snap->meta);
  if (!cov.complete())
    err << "megascope: WARNING: " << cov.indexed << " of " << cov.requested
        << " TU(s) indexed cleanly (" << cov.partial << " partial, "
        << cov.failed << " failed); results may be incomplete\n";
  snap->summary.nodes = snap->graph.nodeCount();
  snap->summary.edges = snap->graph.edgeCount();
  snap->summary.callSites = snap->cfIndex.size();
  snap->summary.channelSites = snap->channels.size();
  snap->loaded = kSectionAll;
  if (common.verbose) {
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    err << "megascope: baked " << files->size() << " TU(s) in memory ("
        << snap->summary.nodes << " nodes, " << snap->summary.edges
        << " edges, " << snap->summary.callSites << " call sites) in "
        << llvm::format("%.1f", ms) << " ms\n";
  }
  snapOut = snap;
  return kExitResults;
}

// ============================================================================
// merge
// ============================================================================

namespace {

struct MergeOpts {
  std::string output;
  std::vector<std::string> inputs;
  std::vector<std::string> inputLists;
  bool allowMismatch = false;
  bool noWait = false;
};

llvm::Expected<MergeOpts> parseMergeFlags(llvm::ArrayRef<std::string> argv) {
  MergeOpts o;
  for (size_t i = 0; i < argv.size(); ++i) {
    llvm::StringRef a = argv[i];
    if (!a.starts_with("-")) {
      o.inputs.push_back(a.str());
      continue;
    }
    if (a == "-o") {
      if (i + 1 >= argv.size())
        return usageError("flag '-o' requires a value");
      o.output = argv[++i];
      continue;
    }
    if (a.starts_with("-o") && !a.starts_with("--")) {
      o.output = a.drop_front(2).str();
      continue;
    }
    if (!a.starts_with("--"))
      return usageError("unknown flag '" + a + "'");

    auto [rawKey, inlineVal] = a.drop_front(2).split('=');
    const bool hasInline = a.contains('=');
    std::string key = rawKey.str();
    std::replace(key.begin(), key.end(), '_', '-');

    if (key == "allow-mismatch" || key == "no-wait") {
      bool *flag = (key == "allow-mismatch") ? &o.allowMismatch : &o.noWait;
      if (hasInline) {
        if (inlineVal == "true" || inlineVal == "1")
          *flag = true;
        else if (inlineVal == "false" || inlineVal == "0")
          *flag = false;
        else
          return usageError("flag '--" + key + "' expects true or false");
      } else {
        *flag = true;
      }
      continue;
    }

    if (key == "output" || key == "input" || key == "input-list") {
      llvm::StringRef value;
      if (hasInline) {
        value = inlineVal;
      } else if (i + 1 < argv.size() &&
                 !llvm::StringRef(argv[i + 1]).starts_with("--")) {
        value = argv[++i];
      } else {
        return usageError("flag '--" + key + "' requires a value");
      }
      if (key == "output")
        o.output = value.str();
      else if (key == "input")
        o.inputs.push_back(value.str());
      else if (key == "input-list")
        o.inputLists.push_back(value.str());
      continue;
    }

    return usageError("unknown flag '" + a + "'");
  }
  return o;
}

void printMergeHelp(llvm::raw_ostream &os) {
  os << "Usage: vycor-cpp megascope merge --output <out.vycs> [in.vycs ...] "
        "[flags]\n\n"
        "Combine per-TU or per-target .vycs snapshot shards into one merged\n"
        "index. Overlapping TUs (e.g. from diamond target dependencies) are\n"
        "deduplicated idempotently.\n\n"
        "Flags:\n"
        "  --output, -o <file>   Output .vycs index path (--index is an alias)\n"
        "  --input <file>        Input .vycs shard path (repeatable; positional\n"
        "                        arguments are also accepted)\n"
        "  --input-list <file>   File listing input .vycs paths, one per line\n"
        "                        ('-' = stdin, '#' comments; repeatable)\n"
        "  --entry-point <name>  Override merged entry points (repeatable)\n"
        "  --allow-mismatch      Allow merging shards whose build configuration\n"
        "                        or bake environment differs\n"
        "  --no-wait             Fail immediately if another writer holds the\n"
        "                        output index lock\n"
        "  --pretty              Indent JSON summary output\n"
        "  -v, --verbose         Progress on stderr\n";
}

} // namespace

int runMergeVerb(llvm::ArrayRef<std::string> args, llvm::StringRef indexFlag,
                 bool pretty, bool verbose,
                 const std::vector<std::string> &entryPoints,
                 std::istream &in, llvm::raw_ostream &out,
                 llvm::raw_ostream &err) {
  auto parsed = parseMergeFlags(args);
  if (!parsed) {
    err << "megascope merge: " << llvm::toString(parsed.takeError()) << "\n";
    return kExitUsage;
  }
  MergeOpts o = std::move(*parsed);
  if (!indexFlag.empty() && !o.output.empty() && indexFlag != o.output) {
    err << "megascope merge: cannot specify both --output and --index with "
           "different paths\n";
    return kExitUsage;
  }
  if (o.output.empty())
    o.output = indexFlag.str();
  if (o.output.empty()) {
    err << "megascope merge: --output <file> (or --index <file>) is required\n";
    return kExitUsage;
  }

  bool stdinConsumed = false;
  for (const std::string &listPath : o.inputLists) {
    std::string content;
    if (listPath == "-") {
      if (stdinConsumed) {
        err << "megascope merge: '--input-list -' can only be specified once\n";
        return kExitUsage;
      }
      stdinConsumed = true;
      content.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    } else {
      auto buf = llvm::MemoryBuffer::getFile(listPath);
      if (!buf) {
        err << "megascope merge: cannot read --input-list " << listPath << ": "
            << buf.getError().message() << "\n";
        return kExitUsage;
      }
      content = (*buf)->getBuffer().str();
    }
    llvm::StringRef rest(content);
    while (!rest.empty()) {
      auto [line, next] = rest.split('\n');
      rest = next;
      llvm::StringRef trimmed = line.split('#').first.trim();
      if (!trimmed.empty())
        o.inputs.push_back(trimmed.str());
    }
  }

  // Deduplicate identical input paths while preserving order.
  std::vector<std::string> uniqueInputs;
  uniqueInputs.reserve(o.inputs.size());
  std::unordered_set<std::string> seenInputs;
  for (const auto &inp : o.inputs) {
    if (seenInputs.insert(inp).second)
      uniqueInputs.push_back(inp);
  }
  o.inputs = std::move(uniqueInputs);

  if (o.inputs.empty()) {
    err << "megascope merge: at least one input .vycs index is required (via "
           "positional arguments, --input, or --input-list)\n";
    return kExitUsage;
  }

  const auto t0 = std::chrono::steady_clock::now();
  std::string lockError;
  bool busy = false;
  auto writeLock = IndexWriteLock::acquire(
      o.output, !o.noWait, &lockError, &busy,
      [&](const std::string &lock) {
        err << "megascope merge: waiting for another writer holding " << lock
            << " (pass --no-wait to fail instead)...\n";
      });
  if (!writeLock && busy) {
    err << "megascope merge: " << lockError << "\n";
    return kExitIndex;
  }
  if (!writeLock) {
    err << "megascope merge: WARNING: " << lockError
        << " — continuing without the write lock\n";
  } else {
    removeStaleAtomicTemps(o.output);
  }

  const bool useMegascopeMerge =
      MegascopeBridge::isMegascopeSnapshotPath(o.output) ||
      std::any_of(o.inputs.begin(), o.inputs.end(), [](const std::string &inp) {
        return MegascopeBridge::isMegascopeSnapshotPath(inp) ||
               MegascopeBridge::isMegascopeShardJsonPath(inp);
      });

  if (useMegascopeMerge) {
    std::string mergeError;
    if (!MegascopeBridge::mergeSnapshots(o.output, o.inputs, entryPoints, {},
                                         &mergeError)) {
      err << "megascope merge: " << mergeError << "\n";
      return kExitIndex;
    }

    int64_t filesCount = 0, indexedCount = 0, partialCount = 0, failedCount = 0;
    int64_t nodesCount = 0, edgesCount = 0, callSitesCount = 0,
            channelSitesCount = 0;
    std::string infoOut, infoErr;
    if (MegascopeBridge::runVerbJson("info", llvm::json::Object(), o.output, "",
                                     infoOut, infoErr) == 0) {
      if (auto parsedInfo = llvm::json::parse(infoOut)) {
        if (const auto *root = parsedInfo->getAsObject()) {
          if (const auto *scope = root->getObject("indexScope")) {
            filesCount = scope->getInteger("requested").value_or(0);
            indexedCount = scope->getInteger("indexed").value_or(0);
            partialCount = scope->getInteger("partial").value_or(0);
            failedCount = scope->getInteger("failed").value_or(0);
          }
          if (const auto *sum = root->getObject("summary")) {
            if (filesCount == 0) {
              filesCount = sum->getInteger("module_count")
                               .value_or(sum->getInteger("modules").value_or(0));
            }
            nodesCount = sum->getInteger("entity_count")
                             .value_or(sum->getInteger("entities").value_or(0));
            edgesCount =
                sum->getInteger("call_edge_count")
                    .value_or(sum->getInteger("call_edges").value_or(0));
            callSitesCount =
                sum->getInteger("context_count")
                    .value_or(sum->getInteger("context_records").value_or(0));
            channelSitesCount = sum->getInteger("channel_sites").value_or(0);
          }
        }
      }
    }

    if (verbose) {
      const double ms = std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
      err << "megascope merge: merged " << o.inputs.size() << " shard(s) ("
          << filesCount << " modules/TUs, " << nodesCount << " entities, "
          << edgesCount << " edges, " << callSitesCount << " call sites) into "
          << o.output << " in " << llvm::format("%.1f", ms) << " ms\n";
    }

    llvm::json::Object summary;
    summary["index"] = toIndexText(o.output);
    summary["mode"] = "merge";
    summary["inputs"] = static_cast<int64_t>(o.inputs.size());
    summary["files"] = filesCount;
    summary["indexed"] = indexedCount;
    summary["partial"] = partialCount;
    summary["failed"] = failedCount;
    summary["nodes"] = nodesCount;
    summary["edges"] = edgesCount;
    summary["call_sites"] = callSitesCount;
    summary["channel_sites"] = channelSitesCount;
    writeJson(out, llvm::json::Value(std::move(summary)), pretty);
    return kExitResults;
  }

  SnapshotIO::MergeOptions mergeOpts;
  mergeOpts.allowMismatch = o.allowMismatch;
  mergeOpts.entryPoints = entryPoints;
  std::string mergeError;
  auto merged = SnapshotIO::merge(o.inputs, mergeOpts, &mergeError);
  if (!merged) {
    err << "megascope merge: " << mergeError << "\n";
    return kExitIndex;
  }

  std::string saveError;
  if (!SnapshotIO::save(o.output, merged->graph, merged->cfIndex, merged->meta,
                        merged->channels, &saveError)) {
    err << "megascope merge: cannot save merged index to " << o.output << ": "
        << saveError << "\n";
    return kExitIndex;
  }

  const IndexCoverage cov = coverageOf(merged->meta);
  if (verbose) {
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    err << "megascope merge: merged " << o.inputs.size() << " shard(s) ("
        << merged->meta.files.size() << " TUs, " << merged->summary.nodes
        << " nodes, " << merged->summary.edges << " edges, "
        << merged->summary.callSites << " call sites) into " << o.output
        << " in " << llvm::format("%.1f", ms) << " ms\n";
  }

  llvm::json::Object summary;
  summary["index"] = toIndexText(o.output);
  summary["mode"] = "merge";
  summary["inputs"] = static_cast<int64_t>(o.inputs.size());
  summary["files"] = static_cast<int64_t>(merged->meta.files.size());
  summary["indexed"] = static_cast<int64_t>(cov.indexed);
  summary["partial"] = static_cast<int64_t>(cov.partial);
  summary["failed"] = static_cast<int64_t>(cov.failed);
  summary["nodes"] = static_cast<int64_t>(merged->summary.nodes);
  summary["edges"] = static_cast<int64_t>(merged->summary.edges);
  summary["call_sites"] = static_cast<int64_t>(merged->summary.callSites);
  summary["channel_sites"] = static_cast<int64_t>(merged->summary.channelSites);
  writeJson(out, llvm::json::Value(std::move(summary)), pretty);
  return kExitResults;
}

// ============================================================================
// dump
// ============================================================================

static llvm::json::Object dumpRecord(const CallSiteContext &ctx) {
  llvm::json::Object rec;
  rec["callerName"] = ctx.callerName;
  rec["callerUsr"] = ctx.callerUsr;
  rec["calleeName"] = ctx.calleeName;
  rec["calleeUsr"] = ctx.calleeUsr;
  rec["callSite"] = ctx.callSite;
  rec["tu"] = ctx.tuPath;
  rec["callerNoexcept"] = noexceptSpecToString(ctx.callerNoexcept);
  rec["insideCatchBlock"] = ctx.insideCatchBlock;
  llvm::json::Array scopes;
  for (const auto &scope : ctx.enclosingTryCatches)
    scopes.push_back(serializeTryCatchScope(scope));
  rec["enclosingTryCatches"] = std::move(scopes);
  llvm::json::Array guards;
  for (const auto &g : ctx.enclosingGuards)
    guards.push_back(serializeGuard(g));
  rec["enclosingGuards"] = std::move(guards);
  llvm::json::Array locals;
  for (const auto &l : ctx.liveRaiiLocals)
    locals.push_back(serializeRaiiLocal(l));
  rec["liveRaiiLocals"] = std::move(locals);
  return rec;
}

/// `dump`: every call-site context, then every channel site, streamed —
/// ndjson writes a {"_summary":...} line then one record per line, each
/// tagged "kind": "call_site" | "channel_site"; json writes one document
/// {callSiteCount, callSites: [...], channelSiteCount, channelSites:
/// [...]} through llvm::json::OStream. Either way the materialized index
/// is never held whole (ControlFlowIndex::forEachContext).
static int runDump(const SnapshotData &snap, const CommonOpts &common,
                   OutputFormat format, llvm::raw_ostream &out,
                   llvm::raw_ostream &err) {
  if (!common.rest.empty()) {
    err << "megascope dump: unexpected argument '" << common.rest.front()
        << "'\n";
    return kExitUsage;
  }
  if (format == OutputFormat::Tsv) {
    err << "megascope dump: records are nested; --format tsv is not "
           "available (use ndjson or json)\n";
    return kExitUsage;
  }
  const auto channelSites = snap.channels.allSites();
  const int64_t callSiteCount = static_cast<int64_t>(snap.cfIndex.size());
  const int64_t channelSiteCount = static_cast<int64_t>(channelSites.size());

  if (format == OutputFormat::Ndjson) {
    llvm::json::Object summary;
    summary["callSiteCount"] = callSiteCount;
    summary["channelSiteCount"] = channelSiteCount;
    llvm::json::Object head;
    head["_summary"] = std::move(summary);
    out << llvm::json::Value(std::move(head)) << "\n";
    snap.cfIndex.forEachContext([&](const CallSiteContext &ctx) {
      llvm::json::Object rec = dumpRecord(ctx);
      rec["kind"] = "call_site";
      out << llvm::json::Value(std::move(rec)) << "\n";
    });
    for (const auto &site : channelSites) {
      llvm::json::Value v = serializeChannelSite(site);
      llvm::json::Object rec = std::move(*v.getAsObject());
      rec["kind"] = "channel_site";
      out << llvm::json::Value(std::move(rec)) << "\n";
    }
  } else {
    llvm::json::OStream J(out, common.pretty ? 2 : 0);
    J.object([&] {
      J.attribute("callSiteCount", callSiteCount);
      J.attributeArray("callSites", [&] {
        snap.cfIndex.forEachContext([&](const CallSiteContext &ctx) {
          J.value(llvm::json::Value(dumpRecord(ctx)));
        });
      });
      J.attribute("channelSiteCount", channelSiteCount);
      J.attributeArray("channelSites", [&] {
        for (const auto &site : channelSites)
          J.value(serializeChannelSite(site));
      });
    });
    out << "\n";
  }
  return callSiteCount + channelSiteCount > 0 ? kExitResults : kExitEmpty;
}

static llvm::Expected<llvm::json::Object>
parseGenericMegascopeArgs(llvm::ArrayRef<std::string> argv,
                          llvm::json::Object seed) {
  llvm::json::Object args = std::move(seed);
  for (size_t i = 0; i < argv.size(); ++i) {
    llvm::StringRef a = argv[i];
    if (!a.starts_with("--") || a.size() == 2)
      return usageError("unexpected argument '" + a + "'");
    llvm::StringRef body = a.drop_front(2);
    const bool hasInline = body.contains('=');
    auto [rawKey, inlineVal] = body.split('=');
    std::string key = canonicalToolName(rawKey);
    if (!hasInline &&
        (i + 1 >= argv.size() ||
         llvm::StringRef(argv[i + 1]).starts_with("--"))) {
      args[key] = true;
      continue;
    }
    llvm::StringRef val = hasInline ? inlineVal : llvm::StringRef(argv[++i]);
    if (val == "true")
      args[key] = true;
    else if (val == "false")
      args[key] = false;
    else
      args[key] = lookupText(val);
  }
  return args;
}

int runMegascopeQueryVerb(llvm::ArrayRef<std::string> args,
                          llvm::raw_ostream &out, llvm::raw_ostream &err,
                          std::istream &in) {
  if (args.empty()) {
    printVerbHelp(err);
    return kExitUsage;
  }
  const llvm::StringRef verb = args.front();
  llvm::ArrayRef<std::string> tail = args.drop_front();
  if (verb == "help" || verb == "--help" || verb == "-h") {
    printVerbHelp(out);
    return kExitResults;
  }

  const std::vector<ToolEntry> tools = getRegisteredTools();

  std::string toolName;
  llvm::StringRef typed = verb; // what to name in "unknown tool"
  if (verb == "call") {
    if (tail.empty() || llvm::StringRef(tail.front()).starts_with("-")) {
      err << "megascope call: expected a tool name, e.g. `megascope call "
             "get_callers --args '{\"name\":\"f\"}'`\n";
      return kExitUsage;
    }
    typed = tail.front();
    toolName = canonicalToolName(typed);
    tail = tail.drop_front();
  } else if (verb != "tools" && verb != "info" && verb != "batch" &&
             verb != "dump" && verb != "diff" && verb != "merge") {
    toolName = canonicalToolName(verb);
  }

  auto common = splitCommonFlags(tail, verb == "info");
  if (!common) {
    err << "megascope " << verb << ": " << llvm::toString(common.takeError())
        << "\n";
    return kExitUsage;
  }

  // If the target index (or diff --before/--after) is a megascope .mgsc/.pyrx
  // binary snapshot or .shard.json, delegate to libmegascope via MegascopeBridge.
  if (!common->ephemeral() && verb != "tools" && verb != "merge" &&
      !common->help) {
    if (verb == "diff") {
      llvm::json::Object diffArgs;
      if (auto parsedArgs =
              parseGenericMegascopeArgs(common->rest, llvm::json::Object())) {
        diffArgs = std::move(*parsedArgs);
      }
      auto beforeStr = diffArgs.getString("before");
      auto afterStr = diffArgs.getString("after");
      if ((beforeStr && MegascopeBridge::isMegascopeSnapshotPath(*beforeStr)) ||
          (afterStr && MegascopeBridge::isMegascopeSnapshotPath(*afterStr))) {
        diffArgs["format"] =
            common->format.empty() ? "json" : common->format;
        diffArgs["pretty"] = common->pretty;
        std::string outStr, errStr;
        int rc = MegascopeBridge::runVerbJson("diff", diffArgs, "", "", outStr,
                                              errStr);
        if (!outStr.empty())
          out << outStr;
        if (!errStr.empty())
          err << "megascope diff: " << errStr << "\n";
        return rc;
      }
    } else {
      const char *envIdx = std::getenv("VYCOR_INDEX");
      std::string candidateIndex = resolveIndexPath(
          common->index, envIdx ? envIdx : "", common->buildPath);
      if (MegascopeBridge::isMegascopeSnapshotPath(candidateIndex) ||
          MegascopeBridge::isMegascopeShardJsonPath(candidateIndex)) {
        const char *stray =
            !common->skipPaths.empty()          ? "--skip-paths"
            : !common->collapsePaths.empty()    ? "--collapse-paths"
            : !common->threads.empty()          ? "--threads"
            : !common->orgConfig.empty()        ? "--org-config"
            : common->hasFixedCompileArgs       ? "-- <compile-flags>"
                                                : nullptr;
        if (stray) {
          err << "megascope " << verb << ": " << stray
              << " only applies with --source/--source-list/--source-re (the "
                 "in-memory bake); a saved index carries its own\n";
          return kExitUsage;
        }
        if (verb == "batch") {
          if (!common->argsJson.empty()) {
            err << "megascope batch: --args only applies to tool queries\n";
            return kExitUsage;
          }
          if (!common->rest.empty()) {
            err << "megascope batch: unexpected argument '"
                << common->rest.front() << "'\n";
            return kExitUsage;
          }
        }
        if (!llvm::sys::fs::exists(candidateIndex)) {
          err << "megascope: no index at " << candidateIndex << "\n";
          return kExitIndex;
        }
        if (verb == "batch") {
          std::string shardJsonContent;
          std::string indexPathArg;
          if (MegascopeBridge::isMegascopeShardJsonPath(candidateIndex)) {
            auto bufOrErr = llvm::MemoryBuffer::getFile(candidateIndex);
            if (!bufOrErr) {
              err << "megascope: cannot read shard JSON " << candidateIndex
                  << "\n";
              return kExitIndex;
            }
            shardJsonContent = (*bufOrErr)->getBuffer().str();
          } else {
            indexPathArg = candidateIndex;
            std::string probeOut, probeErr;
            if (MegascopeBridge::runVerbJson("info", llvm::json::Object(),
                                             indexPathArg, "", probeOut,
                                             probeErr) == kExitIndex) {
              err << "megascope: cannot load index " << candidateIndex << ": "
                  << probeErr << "\n";
              return kExitIndex;
            }
          }

          std::string line;
          size_t lineNo = 0;
          while (std::getline(in, line)) {
            ++lineNo;
            if (llvm::StringRef(line).trim().empty())
              continue;
            llvm::json::Object resp;
            auto emit = [&]() {
              out << llvm::json::Value(std::move(resp)) << "\n";
              out.flush();
            };
            auto batchError = [&](const llvm::Twine &msg) {
              resp["error"] = ("line " + llvm::Twine(lineNo) + ": " + msg).str();
              resp["status"] = resultStatusName(ResultStatus::UsageError);
              resp["exit"] = static_cast<int64_t>(kExitUsage);
              emit();
            };

            auto parsed = llvm::json::parse(line);
            if (!parsed) {
              batchError(llvm::toString(parsed.takeError()));
              continue;
            }
            const auto *req = parsed->getAsObject();
            if (!req) {
              batchError("request must be a JSON object");
              continue;
            }
            if (const auto *id = req->get("id"))
              resp["id"] = *id;
            auto reqTool = req->getString("tool");
            if (!reqTool) {
              batchError("missing 'tool'");
              continue;
            }
            const std::string canon = canonicalToolName(*reqTool);
            resp["tool"] = canon;
            llvm::json::Object args;
            if (const auto *a = req->getObject("args"))
              args = *a;
            else if (const auto *a = req->getObject("arguments"))
              args = *a;
            if (auto cid = args.getString("channel_id")) {
              if (!args.get("channel"))
                args["channel"] = *cid;
            }
            args["format"] = "json";
            args["pretty"] = false;
            if (!common->entryPoints.empty()) {
              llvm::json::Array eps;
              for (const auto &ep : common->entryPoints)
                eps.push_back(ep);
              args["entry_points"] = std::move(eps);
            }

            std::string outStr, errStr;
            int rc = MegascopeBridge::runVerbJson(
                canon, args, indexPathArg, shardJsonContent, outStr, errStr);
            if (outStr.empty()) {
              batchError(errStr.empty() ? ("unknown tool '" + *reqTool + "'")
                                        : llvm::Twine(errStr));
              continue;
            }
            auto payload = llvm::json::parse(outStr);
            if (!payload) {
              batchError(llvm::toString(payload.takeError()));
              continue;
            }
            std::string statusStr = resultStatusName(statusOf(*payload));
            if (const auto *pObj = payload->getAsObject()) {
              if (auto s = pObj->getString("status"))
                statusStr = s->str();
            }
            resp["status"] = statusStr;
            resp["exit"] = static_cast<int64_t>(rc);
            resp["result"] = std::move(*payload);
            emit();
          }
          return kExitResults;
        }

        llvm::json::Object seed;
        if (!common->argsJson.empty()) {
          auto parsed = llvm::json::parse(common->argsJson);
          if (!parsed || !parsed->getAsObject()) {
            err << "megascope " << typed << ": --args must be a JSON object\n";
            return kExitUsage;
          }
          seed = std::move(*parsed->getAsObject());
        }
        auto parsedArgs = parseGenericMegascopeArgs(common->rest, std::move(seed));
        if (!parsedArgs) {
          err << "megascope " << typed << ": "
              << llvm::toString(parsedArgs.takeError()) << "\n";
          return kExitUsage;
        }
        llvm::json::Object mgscArgs = std::move(*parsedArgs);
        if (auto cid = mgscArgs.getString("channel_id")) {
          if (!mgscArgs.get("channel"))
            mgscArgs["channel"] = *cid;
        }
        mgscArgs["format"] =
            common->format.empty() ? "json" : common->format;
        mgscArgs["pretty"] = common->pretty;
        if (!common->entryPoints.empty()) {
          llvm::json::Array eps;
          for (const auto &ep : common->entryPoints)
            eps.push_back(ep);
          mgscArgs["entry_points"] = std::move(eps);
        }

        std::string effectiveVerb =
            toolName.empty() ? verb.str() : toolName;
        std::string outStr, errStr;
        int rc = 0;
        if (MegascopeBridge::isMegascopeShardJsonPath(candidateIndex)) {
          auto bufOrErr = llvm::MemoryBuffer::getFile(candidateIndex);
          if (!bufOrErr) {
            err << "megascope: cannot read shard JSON " << candidateIndex
                << "\n";
            return kExitIndex;
          }
          rc = MegascopeBridge::runVerbJson(
              effectiveVerb, mgscArgs, "", (*bufOrErr)->getBuffer(), outStr,
              errStr);
        } else {
          rc = MegascopeBridge::runVerbJson(effectiveVerb, mgscArgs,
                                            candidateIndex, "", outStr, errStr);
        }
        if (!outStr.empty())
          out << outStr;
        if (!errStr.empty())
          err << "megascope " << typed << ": " << errStr << "\n";
        return rc;
      }
    }
  }

  const ToolEntry *tool = nullptr;
  if (!toolName.empty()) {
    for (const auto &t : tools)
      if (t.name == toolName)
        tool = &t;
    if (!tool) {
      err << "megascope: unknown verb or tool '" << typed
          << "'. Verbs: index, merge, tools, info, batch, dump, diff, "
             "call, <tool>; "
             "run `vycor-cpp megascope tools` for the tool list.\n";
      return kExitUsage;
    }
  }

  if (common->help) {
    if (tool)
      printToolHelp(*tool, out);
    else if (verb == "merge")
      printMergeHelp(out);
    else
      printVerbHelp(out);
    return kExitResults;
  }
  if (verb == "tools")
    return runTools(tools, *common, out, err);
  if (verb == "merge") {
    if (common->ephemeral() || !common->buildPath.empty() ||
        common->hasFixedCompileArgs || !common->skipPaths.empty() ||
        !common->collapsePaths.empty() || !common->threads.empty() ||
        !common->orgConfig.empty() || !common->argsJson.empty()) {
      err << "megascope merge: merges saved .vycs indexes; it does not take "
             "--build-path, --args, or the ephemeral source/bake flags\n";
      return kExitUsage;
    }
    if (!common->format.empty() && common->format != "json") {
      err << "megascope merge: only --format json is supported (got '"
          << common->format << "')\n";
      return kExitUsage;
    }
    return runMergeVerb(common->rest, common->index, common->pretty,
                        common->verbose, common->entryPoints, in, out, err);
  }
  if (verb == "diff") {
    if (common->ephemeral() || !common->index.empty() ||
        !common->buildPath.empty() || common->hasFixedCompileArgs ||
        !common->skipPaths.empty() || !common->collapsePaths.empty() ||
        !common->threads.empty() || !common->orgConfig.empty() ||
        !common->argsJson.empty()) {
      err << "megascope diff: takes --before and --after indexes, not "
             "--index, --build-path, --args, or the ephemeral selection/bake "
             "flags\n";
      return kExitUsage;
    }
    auto fmt = parseFormat(common->format.empty() ? "json" : common->format);
    if (!fmt) {
      err << "megascope diff: " << llvm::toString(fmt.takeError()) << "\n";
      return kExitUsage;
    }
    return runDiffVerb(common->rest, *fmt, common->pretty,
                       common->entryPoints, out, err);
  }

  auto format = parseFormat(
      common->format.empty() ? (verb == "dump" ? "ndjson" : "json")
                             : common->format);
  if (!format) {
    err << "megascope " << verb << ": " << llvm::toString(format.takeError())
        << "\n";
    return kExitUsage;
  }

  // Tool arguments are validated before the index is loaded so a usage
  // error never pays the load tax.
  llvm::json::Object toolArgs;
  if (tool) {
    llvm::json::Object seed;
    if (!common->argsJson.empty()) {
      auto parsed = llvm::json::parse(common->argsJson);
      if (!parsed || !parsed->getAsObject()) {
        err << "megascope " << hyphenated(tool->name)
            << ": --args must be a JSON object";
        if (!parsed)
          err << ": " << llvm::toString(parsed.takeError());
        err << "\n";
        return kExitUsage;
      }
      seed = std::move(*parsed->getAsObject());
    }
    if (tool->name == "impact_of_change") {
      int rc = seedImpactPatch(common->rest, seed, in, err);
      if (rc != kExitResults)
        return rc;
    }
    auto parsed = parseToolArgs(*tool, common->rest, std::move(seed));
    if (!parsed) {
      err << "megascope " << hyphenated(tool->name) << ": "
          << llvm::toString(parsed.takeError()) << "\n";
      return kExitUsage;
    }
    toolArgs = std::move(*parsed);
  } else {
    if (!common->argsJson.empty()) {
      err << "megascope " << verb << ": --args only applies to tool queries\n";
      return kExitUsage;
    }
    if (!common->rest.empty()) {
      err << "megascope " << verb << ": unexpected argument '"
          << common->rest.front() << "'\n";
      return kExitUsage;
    }
  }

  // Decode only what the verb reads: info wants the meta section, batch
  // may run any tool, dump the contexts and channel sites, a single tool
  // declares its needs (§3.1.1).
  unsigned needs = kSectionAll;
  if (verb == "info")
    needs = 0;
  else if (verb == "dump")
    needs = kSectionControlFlow | kSectionChannels;
  else if (verb != "batch")
    needs = tool->needs;

  // Deliberately leaked: a one-shot process never frees the indexes.
  // Destroying maps holding millions of entries costs seconds at exit
  // (measured 2-3.5 s against a 5 s load on the 938-TU testbed) and buys
  // nothing — the OS reclaims the pages.
  SnapshotData *snap = nullptr;
  std::string indexPath;
  if (common->ephemeral()) {
    int rc = bakeEphemeral(*common, verb, in, err, snap);
    if (rc != kExitResults)
      return rc;
  } else {
    // The bake qualifiers mean nothing against a saved index; silently
    // ignoring them would hide a forgotten --source.
    const char *stray = !common->skipPaths.empty()          ? "--skip-paths"
                        : !common->collapsePaths.empty()    ? "--collapse-paths"
                        : !common->threads.empty()          ? "--threads"
                        : !common->orgConfig.empty()        ? "--org-config"
                        : common->hasFixedCompileArgs       ? "-- <compile-flags>"
                                                            : nullptr;
    if (stray) {
      err << "megascope " << verb << ": " << stray
          << " only applies with --source/--source-list/--source-re (the "
             "in-memory bake); a saved index carries its own\n";
      return kExitUsage;
    }
    const char *envIndex = std::getenv("VYCOR_INDEX");
    indexPath = resolveIndexPath(common->index, envIndex ? envIndex : "",
                                 common->buildPath);
    if (!llvm::sys::fs::exists(indexPath)) {
      err << "megascope: no index at " << indexPath
          << " (run `vycor-cpp megascope index --build-path <dir> ...` "
             "first, pass --index, or pass --source to bake in memory)\n";
      return kExitIndex;
    }
    SnapshotLoadStats loadStats;
    auto *snapHolder = retainUntilExit(new std::optional<SnapshotData>(
        SnapshotIO::load(indexPath, &loadStats, LoadMode::ReadOnly, needs)));
    if (!*snapHolder) {
      err << "megascope: cannot load index " << indexPath << ": "
          << loadStats.error << " (re-run `megascope index`)\n";
      return kExitIndex;
    }
    // info decodes the meta alone but vouches for the whole file: every
    // section's checksum is verified.
    std::string damaged;
    if (verb == "info" && !SnapshotIO::verify(indexPath, &damaged)) {
      err << "megascope: cannot load index " << indexPath << ": " << damaged
          << " (re-run `megascope index`)\n";
      return kExitIndex;
    }
    snap = &**snapHolder;
    if (common->verbose) {
      err << "megascope: loaded " << indexPath << " ("
          << snap->summary.nodes << " nodes, " << snap->summary.edges
          << " edges, " << snap->summary.callSites << " call sites) in "
          << llvm::format("%.1f", loadStats.totalMs) << " ms:";
      for (const auto &sec : loadStats.sections) {
        err << " " << sec.name << " ";
        if (sec.skipped)
          err << "skipped/";
        else
          err << llvm::format("%.1f", sec.ms) << " ms/";
        err << (sec.bytes >> 10) << " KiB";
      }
      err << "\n";
    }
  }

  if (verb == "info")
    return runInfo(*snap, indexPath, *common, *format, out, err);
  if (verb == "dump")
    return runDump(*snap, *common, *format, out, err);

  ControlFlowOracle oracle(snap->graph, snap->cfIndex);
  QueryCache cache;
  // --entry-point on the query, else what the index was baked with (v8
  // meta), else main.
  std::vector<std::string> entryPoints = common->entryPoints;
  if (entryPoints.empty())
    entryPoints = snap->meta.entryPoints;
  if (entryPoints.empty())
    entryPoints.push_back("main");
  // Flag and meta spellings are raw bytes; tools look them up and echo
  // them as index text (Utf8.h).
  for (auto &e : entryPoints)
    e = lookupText(e);
  ToolContext ctx{snap->graph,  oracle,          snap->cfIndex,
                  entryPoints, &snap->channels, &cache,
                  &snap->summary};
  // What every payload says about its index (docs/result-contract.md):
  // an in-memory bake is fresh by construction and has no saved bake to
  // cite; a loaded index is taken as-is, unchecked.
  ctx.facts = IndexFacts::of(snap->meta, common->ephemeral()
                                             ? IndexFreshness::Baked
                                             : IndexFreshness::Unchecked);

  if (verb == "batch")
    return runBatch(tools, ctx, in, out);

  llvm::json::Value payload = runTool(*tool, toolArgs, ctx);
  return emitToolResult(payload, tool->recordsKey, *format, common->pretty,
                        out, err, hyphenated(tool->name));
}

} // namespace vycor
