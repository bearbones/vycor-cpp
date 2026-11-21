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
#include "vycor/metrics/TargetOwners.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

#include <algorithm>

namespace vycor {

namespace {

// Absolute against `base` when relative, `.` and `..` removed lexically.
std::string normalPath(llvm::StringRef path, llvm::StringRef base) {
  llvm::SmallString<256> buf;
  if (llvm::sys::path::is_absolute(path) || base.empty()) {
    buf = path;
  } else {
    buf = base;
    llvm::sys::path::append(buf, path);
  }
  llvm::sys::path::remove_dots(buf, /*remove_dot_dot=*/true);
  return std::string(buf.str());
}

} // namespace

bool TargetOwners::parse(llvm::StringRef json, llvm::StringRef baseDir,
                         TargetOwners &out, std::string &error) {
  auto doc = llvm::json::parse(json);
  if (!doc) {
    error = "parse error: " + llvm::toString(doc.takeError());
    return false;
  }
  const llvm::json::Object *root = doc->getAsObject();
  const llvm::json::Array *targets = root ? root->getArray("targets") : nullptr;
  if (!targets) {
    error = "expected an object with a `targets` array";
    return false;
  }
  std::string base = baseDir.str();
  if (auto mapRoot = root->getString("root"))
    base = normalPath(*mapRoot, baseDir);

  TargetOwners owners;
  for (const llvm::json::Value &value : *targets) {
    const llvm::json::Object *target = value.getAsObject();
    auto name = target ? target->getString("name") : std::nullopt;
    if (!name || name->empty()) {
      error = "every target needs a non-empty string `name`";
      return false;
    }
    ++owners.targetCount_;
    const llvm::json::Value *sources = target->get("sources");
    if (!sources)
      continue; // an interface or imported target owns no TU
    const llvm::json::Array *list = sources->getAsArray();
    if (!list) {
      error = "target '" + name->str() + "': `sources` must be an array";
      return false;
    }
    for (const llvm::json::Value &source : *list) {
      auto path = source.getAsString();
      if (!path) {
        error = "target '" + name->str() + "': `sources` must hold strings";
        return false;
      }
      owners.bySource_[normalPath(*path, base)].push_back(name->str());
    }
  }
  for (auto &entry : owners.bySource_) {
    auto &names = entry.second;
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
  }
  out = std::move(owners);
  return true;
}

bool TargetOwners::load(llvm::StringRef path, TargetOwners &out,
                        std::string &error) {
  auto buffer = llvm::MemoryBuffer::getFile(path);
  if (!buffer) {
    error = "cannot read " + path.str() + ": " + buffer.getError().message();
    return false;
  }
  llvm::SmallString<256> abs(path);
  llvm::sys::fs::make_absolute(abs);
  if (!parse((*buffer)->getBuffer(), llvm::sys::path::parent_path(abs), out,
             error)) {
    error = path.str() + ": " + error;
    return false;
  }
  return true;
}

llvm::ArrayRef<std::string>
TargetOwners::ownersOf(llvm::StringRef tuPath) const {
  auto it = bySource_.find(normalPath(tuPath, ""));
  if (it == bySource_.end())
    return {};
  return it->second;
}

} // namespace vycor
