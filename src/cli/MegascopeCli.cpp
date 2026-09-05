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
#include "vycor/callgraph/ControlFlowOracle.h"
#include "vycor/callgraph/Snapshot.h"
#include "vycor/cli/BakeConfig.h"
#include "vycor/cli/SourceSelection.h"
#include "vycor/query/Serialize.h"

#include "clang/Tooling/CompilationDatabase.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <set>
#include <string>
#include <unordered_map>

namespace vycor {

// ============================================================================
// Names and paths
// ============================================================================

std::string defaultIndexPath(llvm::StringRef buildPath) {
  llvm::SmallString<256> p(buildPath.empty() ? llvm::StringRef(".")
                                             : buildPath);
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
  return !verb.empty() && !verb.starts_with("-") && verb != "index" &&
         verb != "serve";
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
      llvm::json::Value item(value->str());
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
      args[key] = value->str();
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
        "\nExit codes: 0 results, 1 empty, 2 usage, 3 index missing or\n"
        "unreadable, 4 ambiguous identity (candidates on stdout).\n";
}

// ============================================================================
// Output contract
// ============================================================================

namespace {

/// Handler error messages that describe malformed arguments rather than an
/// empty answer: the prefixes the result contract in Tools.h reserves.
bool isUsageMessage(llvm::StringRef msg) {
  return msg.starts_with("Missing") || msg.starts_with("Requires") ||
         msg.starts_with("Invalid");
}

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
  if (auto msg = errorMessage(payload))
    return isUsageMessage(*msg) ? kExitUsage : kExitEmpty;
  if (isAmbiguousResult(payload))
    return kExitAmbiguous;
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
      out << "error\n" << tsvEscape(*msg) << "\n";
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
  std::vector<std::string> rest; // everything else, in order

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
        "            compilation database and save it (--build-path;\n"
        "            --source/--source-list/--source-re/--skip-paths\n"
        "            select TUs, default: the TUs already in the index,\n"
        "            else the whole database; `megascope index --help`)\n"
        "  serve     Bake or warm-start (same flags as index), then serve\n"
        "            the tools over MCP stdio\n"
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
        "\n"
        "Query verbs read the index from --index, $VYCOR_INDEX,\n"
        "<build-path>/.vycor/megascope.vycs, or ./.vycor/megascope.vycs.\n"
        "Given --source/--source-list/--source-re (with --build-path) they\n"
        "bake the selected TUs in memory instead and answer from that; no\n"
        "index is read or written (--skip-paths, --collapse-paths,\n"
        "--threads, and --org-config qualify the bake as for `index`).\n"
        "Run `megascope <tool> --help` for a tool's flags.\n"
        "\n"
        "Exit codes: 0 results, 1 empty, 2 usage, 3 index missing or\n"
        "unreadable, 4 ambiguous identity (candidates on stdout).\n";
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
  if (common.format.empty()) {
    size_t width = 0;
    for (const auto &t : tools)
      width = std::max(width, hyphenated(t.name).size());
    for (const auto &t : tools) {
      std::string verb = hyphenated(t.name);
      out << "  " << verb << std::string(width + 2 - verb.size(), ' ')
          << firstSentence(t.description)
          << (t.handler ? "" : "  [serve only]") << "\n";
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
    o["cli"] = t.handler != nullptr;
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
  o["index"] = indexPath.str();
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
  o["entry_points"] = llvm::json::Array(snap.meta.entryPoints);

  llvm::json::Object cfg;
  cfg["collapse_paths"] = llvm::json::Array(snap.meta.collapsePaths);
  cfg["lock_types"] = llvm::json::Array(snap.meta.lockAllowlist);
  cfg["lock_builtins"] = snap.meta.lockBuiltins;
  llvm::json::Array channelTypes;
  for (const auto &ct : snap.meta.channelTypes) {
    llvm::json::Object c;
    c["type"] = ct.qualifiedTypeName;
    c["category"] = ct.category;
    c["produce"] = llvm::json::Array(ct.produceMethods);
    c["consume"] = llvm::json::Array(ct.consumeMethods);
    channelTypes.push_back(llvm::json::Value(std::move(c)));
  }
  cfg["channel_types"] = std::move(channelTypes);
  o["config"] = std::move(cfg);

  if (common.files) {
    llvm::json::Array files;
    for (const auto &fs : snap.meta.files) {
      llvm::json::Object f;
      f["path"] = fs.path;
      f["mtime_ns"] = static_cast<int64_t>(fs.mtimeNs);
      f["size"] = static_cast<int64_t>(fs.size);
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
    if (it == byName.end() || !it->second->handler) {
      batchError("unknown tool '" + *toolName + "'");
      continue;
    }
    llvm::json::Object args;
    if (const auto *a = req->getObject("args"))
      args = *a;
    else if (const auto *a = req->getObject("arguments"))
      args = *a;
    llvm::json::Value payload = it->second->handler(args, ctx);
    resp["exit"] = static_cast<int64_t>(
        exitCodeFor(payload, it->second->recordsKey));
    resp["result"] = std::move(payload);
    emit();
  }
  return kExitResults;
}

} // namespace

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
  if (common.buildPath.empty()) {
    err << prefix << "--build-path is required with --source/--source-list/"
                     "--source-re\n";
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
  auto compDb = clang::tooling::CompilationDatabase::loadFromDirectory(
      common.buildPath, dbError);
  if (!compDb) {
    err << "megascope: cannot load compilation database from "
        << common.buildPath << ": " << dbError << "\n";
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
    err << "megascope: the selection matches no TU of " << common.buildPath
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
  auto *snap = new SnapshotData;
  snap->graph = std::move(baked.graph);
  snap->cfIndex = std::move(baked.cfIndex);
  snap->channels = std::move(baked.channels);
  snap->meta.collapsePaths = collapsePaths;
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
             verb != "dump") {
    toolName = canonicalToolName(verb);
  }

  const ToolEntry *tool = nullptr;
  if (!toolName.empty()) {
    for (const auto &t : tools)
      if (t.name == toolName)
        tool = &t;
    if (!tool) {
      err << "megascope: unknown verb or tool '" << typed
          << "'. Verbs: index, serve, tools, info, batch, dump, call, "
             "<tool>; "
             "run `vycor-cpp megascope tools` for the tool list.\n";
      return kExitUsage;
    }
    if (!tool->handler) {
      err << "megascope: " << hyphenated(tool->name)
          << " mutates the index and is only available through `serve`\n";
      return kExitUsage;
    }
  }

  auto common = splitCommonFlags(tail, verb == "info");
  if (!common) {
    err << "megascope " << verb << ": " << llvm::toString(common.takeError())
        << "\n";
    return kExitUsage;
  }
  if (common->help) {
    if (tool)
      printToolHelp(*tool, out);
    else
      printVerbHelp(out);
    return kExitResults;
  }
  if (verb == "tools")
    return runTools(tools, *common, out, err);

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
    auto parsed = parseToolArgs(*tool, common->rest, std::move(seed));
    if (!parsed) {
      err << "megascope " << hyphenated(tool->name) << ": "
          << llvm::toString(parsed.takeError()) << "\n";
      return kExitUsage;
    }
    toolArgs = std::move(*parsed);
  } else if (!common->rest.empty()) {
    err << "megascope " << verb << ": unexpected argument '"
        << common->rest.front() << "'\n";
    return kExitUsage;
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
    const char *stray = !common->skipPaths.empty()       ? "--skip-paths"
                        : !common->collapsePaths.empty() ? "--collapse-paths"
                        : !common->threads.empty()       ? "--threads"
                        : !common->orgConfig.empty()     ? "--org-config"
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
    auto *snapHolder = new std::optional<SnapshotData>(
        SnapshotIO::load(indexPath, &loadStats, LoadMode::ReadOnly, needs));
    if (!*snapHolder) {
      err << "megascope: cannot load index " << indexPath
          << " (wrong format version or unreadable; re-run `megascope "
             "index`)\n";
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
  ToolContext ctx{snap->graph,  oracle,          snap->cfIndex,
                  entryPoints, &snap->channels, &cache,
                  &snap->summary};

  if (verb == "batch")
    return runBatch(tools, ctx, in, out);

  llvm::json::Value payload = tool->handler(toolArgs, ctx);
  return emitToolResult(payload, tool->recordsKey, *format, common->pretty,
                        out, err, hyphenated(tool->name));
}

} // namespace vycor
