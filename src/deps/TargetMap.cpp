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

#include "vycor/deps/TargetMap.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>
#include <numeric>

namespace vycor {

namespace {

struct KindName {
  TargetKind kind;
  const char *name;
};

constexpr KindName kKindNames[] = {
    {TargetKind::Executable, "executable"},
    {TargetKind::StaticLibrary, "static_library"},
    {TargetKind::SharedLibrary, "shared_library"},
    {TargetKind::ModuleLibrary, "module_library"},
    {TargetKind::ObjectLibrary, "object_library"},
    {TargetKind::InterfaceLibrary, "interface_library"},
    {TargetKind::External, "external"},
};

/// One target as read, before the name sort fixes the indices.
struct RawTarget {
  std::string name;
  TargetKind kind = TargetKind::StaticLibrary;
  std::vector<std::string> sources;
  std::vector<std::string> headers;
  std::vector<std::string> deps;
};

bool readPathList(const llvm::json::Object &obj, llvm::StringRef key,
                  llvm::StringRef targetName, llvm::StringRef root,
                  std::vector<std::string> &out, std::string &error) {
  const llvm::json::Value *v = obj.get(key);
  if (!v)
    return true;
  const auto *arr = v->getAsArray();
  if (!arr) {
    error = ("target '" + targetName + "': `" + key + "` must be an array")
                .str();
    return false;
  }
  out.reserve(arr->size());
  for (const auto &e : *arr) {
    auto s = e.getAsString();
    if (!s || s->empty()) {
      error = ("target '" + targetName + "': `" + key +
               "` must hold non-empty strings")
                  .str();
      return false;
    }
    out.push_back(normalizeAuditPath(*s, root));
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return true;
}

} // namespace

const char *targetKindName(TargetKind kind) {
  for (const auto &k : kKindNames)
    if (k.kind == kind)
      return k.name;
  return "static_library";
}

std::optional<TargetKind> parseTargetKind(llvm::StringRef name) {
  for (const auto &k : kKindNames)
    if (name == k.name)
      return k.kind;
  return std::nullopt;
}

std::optional<uint32_t> TargetMap::find(llvm::StringRef name) const {
  auto it = std::lower_bound(
      names.begin(), names.end(), name,
      [](const std::string &a, llvm::StringRef b) { return a < b; });
  if (it == names.end() || *it != name)
    return std::nullopt;
  return static_cast<uint32_t>(it - names.begin());
}

bool TargetMap::underRoots(llvm::StringRef path) const noexcept {
  for (const std::string &root : roots) {
    llvm::StringRef r(root);
    while (r.size() > 1 && r.ends_with("/"))
      r = r.drop_back();
    if (path.starts_with(r) &&
        (r == "/" || (path.size() > r.size() && path[r.size()] == '/')))
      return true;
  }
  return false;
}

std::string normalizeAuditPath(llvm::StringRef path, llvm::StringRef base) {
  llvm::SmallString<256> buf;
  if (!llvm::sys::path::is_absolute(path) && !base.empty()) {
    buf = base;
    llvm::sys::path::append(buf, path);
  } else {
    buf = path;
  }
  llvm::sys::path::remove_dots(buf, /*remove_dot_dot=*/true);
  return std::string(buf.str());
}

bool parseTargetMap(const llvm::json::Value &doc, llvm::StringRef baseDir,
                    TargetMap &out, std::string &error) {
  const auto *top = doc.getAsObject();
  if (!top) {
    error = "the target map must be a JSON object";
    return false;
  }
  auto schema = top->getInteger("schema");
  if (!schema || *schema != 1) {
    error = "`schema` must be 1";
    return false;
  }
  out = TargetMap{};
  if (auto g = top->getString("generator"))
    out.generator = g->str();
  if (const llvm::json::Value *form = top->get("depsForm")) {
    auto s = form->getAsString();
    if (!s || (*s != "direct" && *s != "closure")) {
      error = "`depsForm` must be \"direct\" or \"closure\"";
      return false;
    }
    out.depsAreClosure = *s == "closure";
  }
  std::string root = baseDir.str();
  if (const llvm::json::Value *r = top->get("root")) {
    auto s = r->getAsString();
    if (!s || s->empty()) {
      error = "`root` must be a non-empty string";
      return false;
    }
    root = normalizeAuditPath(*s, baseDir);
  }
  if (const llvm::json::Value *rv = top->get("roots")) {
    const auto *arr = rv->getAsArray();
    if (!arr || arr->empty()) {
      error = "`roots` must be a non-empty array of directories";
      return false;
    }
    for (const auto &e : *arr) {
      auto s = e.getAsString();
      if (!s || s->empty()) {
        error = "`roots` must hold non-empty strings";
        return false;
      }
      out.roots.push_back(normalizeAuditPath(*s, root));
    }
  } else {
    out.roots.push_back(normalizeAuditPath(root, ""));
  }
  std::sort(out.roots.begin(), out.roots.end());
  out.roots.erase(std::unique(out.roots.begin(), out.roots.end()),
                  out.roots.end());

  const auto *targets = top->getArray("targets");
  if (!targets) {
    error = "`targets` must be an array";
    return false;
  }
  std::vector<RawTarget> raw;
  raw.reserve(targets->size());
  for (const auto &tv : *targets) {
    const auto *to = tv.getAsObject();
    if (!to) {
      error = "every entry of `targets` must be an object";
      return false;
    }
    RawTarget t;
    auto name = to->getString("name");
    if (!name || name->empty()) {
      error = "every target needs a non-empty `name`";
      return false;
    }
    t.name = name->str();
    auto kind = to->getString("kind");
    if (!kind) {
      error = "target '" + t.name + "': `kind` is required";
      return false;
    }
    auto parsed = parseTargetKind(*kind);
    if (!parsed) {
      error = ("target '" + t.name + "': unknown `kind` '" + *kind + "'").str();
      return false;
    }
    t.kind = *parsed;
    if (!readPathList(*to, "sources", t.name, root, t.sources, error) ||
        !readPathList(*to, "headers", t.name, root, t.headers, error))
      return false;
    if (const llvm::json::Value *dv = to->get("deps")) {
      const auto *arr = dv->getAsArray();
      if (!arr) {
        error = "target '" + t.name + "': `deps` must be an array";
        return false;
      }
      t.deps.reserve(arr->size());
      for (const auto &e : *arr) {
        auto s = e.getAsString();
        if (!s || s->empty()) {
          error = "target '" + t.name + "': `deps` must hold target names";
          return false;
        }
        t.deps.push_back(s->str());
      }
    }
    raw.push_back(std::move(t));
  }

  std::vector<uint32_t> order(raw.size());
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return raw[a].name < raw[b].name;
  });
  for (size_t i = 1; i < order.size(); ++i)
    if (raw[order[i]].name == raw[order[i - 1]].name) {
      error = "duplicate target name '" + raw[order[i]].name + "'";
      return false;
    }

  const size_t n = raw.size();
  out.names.reserve(n);
  out.kinds.reserve(n);
  out.sourceBegin.reserve(n + 1);
  out.headerBegin.reserve(n + 1);
  out.depBegin.reserve(n + 1);
  for (uint32_t i : order)
    out.names.push_back(raw[i].name);
  for (uint32_t i : order) {
    RawTarget &t = raw[i];
    const auto self = static_cast<uint32_t>(out.kinds.size());
    out.kinds.push_back(t.kind);
    out.sourceBegin.push_back(static_cast<uint32_t>(out.sources.size()));
    out.headerBegin.push_back(static_cast<uint32_t>(out.headers.size()));
    out.depBegin.push_back(static_cast<uint32_t>(out.deps.size()));
    for (auto &s : t.sources)
      out.sources.push_back(std::move(s));
    for (auto &h : t.headers)
      out.headers.push_back(std::move(h));
    const size_t depStart = out.deps.size();
    for (const auto &d : t.deps) {
      auto idx = out.find(d);
      if (!idx) {
        error = "target '" + t.name + "' depends on '" + d +
                "', which the map does not declare";
        return false;
      }
      if (*idx != self)
        out.deps.push_back(*idx);
    }
    std::sort(out.deps.begin() + static_cast<ptrdiff_t>(depStart),
              out.deps.end());
    out.deps.erase(
        std::unique(out.deps.begin() + static_cast<ptrdiff_t>(depStart),
                    out.deps.end()),
        out.deps.end());
  }
  out.sourceBegin.push_back(static_cast<uint32_t>(out.sources.size()));
  out.headerBegin.push_back(static_cast<uint32_t>(out.headers.size()));
  out.depBegin.push_back(static_cast<uint32_t>(out.deps.size()));
  return true;
}

bool loadTargetMap(llvm::StringRef path, TargetMap &out, std::string &error) {
  auto buf = llvm::MemoryBuffer::getFile(path);
  if (!buf) {
    error = ("cannot read target map '" + path + "': " +
             buf.getError().message())
                .str();
    return false;
  }
  auto doc = llvm::json::parse((*buf)->getBuffer());
  if (!doc) {
    error = ("target map '" + path + "' is not valid JSON: " +
             llvm::toString(doc.takeError()))
                .str();
    return false;
  }
  llvm::SmallString<256> abs(path);
  llvm::sys::fs::make_absolute(abs);
  std::string detail;
  if (!parseTargetMap(*doc, llvm::sys::path::parent_path(abs), out, detail)) {
    error = ("target map '" + path + "': " + detail).str();
    return false;
  }
  return true;
}

} // namespace vycor
