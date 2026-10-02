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

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vycor {

class StringInterner {
public:
  using Id = uint32_t;

  StringInterner() = default;

  StringInterner(StringInterner &&other) noexcept {
    std::unique_lock lock(other.mutex_);
    strings_ = std::move(other.strings_);
    rebuildIndex();
  }

  StringInterner &operator=(StringInterner &&other) noexcept {
    if (this != &other) {
      std::unique_lock lockOther(other.mutex_);
      std::unique_lock lockThis(mutex_);
      strings_ = std::move(other.strings_);
      rebuildIndex();
    }
    return *this;
  }

  StringInterner(const StringInterner &) = delete;
  StringInterner &operator=(const StringInterner &) = delete;

  Id intern(const std::string &s) {
    {
      std::shared_lock lock(mutex_);
      auto it = index_.find(s);
      if (it != index_.end())
        return it->second;
    }
    std::unique_lock lock(mutex_);
    auto it = index_.find(s);
    if (it != index_.end())
      return it->second;
    Id id = static_cast<Id>(strings_.size());
    strings_.push_back(s);
    index_.emplace(std::string_view(strings_.back()), id);
    return id;
  }

  const std::string &resolve(Id id) const {
    std::shared_lock lock(mutex_);
    return strings_[id];
  }

  std::optional<Id> find(const std::string &s) const {
    std::shared_lock lock(mutex_);
    auto it = index_.find(s);
    if (it != index_.end())
      return it->second;
    return std::nullopt;
  }

  size_t size() const {
    std::shared_lock lock(mutex_);
    return strings_.size();
  }

  /// Visit every interned string in id order: fn(const std::string&) is
  /// called for id 0, 1, 2, ... Snapshot save serializes the table with
  /// this so that a table installed via installStrings reproduces the ids
  /// by position.
  template <typename Fn> void forEachString(Fn fn) const {
    std::shared_lock lock(mutex_);
    for (const auto &s : strings_)
      fn(s);
  }

  /// Bulk-install a string table whose positions become the ids (snapshot
  /// load). Only valid on an empty interner: returns false without
  /// modifying anything when strings have already been interned. Later
  /// intern() calls extend the table normally.
  bool installStrings(std::vector<std::string> strings) {
    std::unique_lock lock(mutex_);
    if (!strings_.empty())
      return false;
    for (auto &s : strings)
      strings_.push_back(std::move(s));
    rebuildIndex();
    return true;
  }

  /// Total bytes of interned string payload (excludes container overhead).
  /// For memory diagnostics; O(n).
  size_t payloadBytes() const {
    std::shared_lock lock(mutex_);
    size_t total = 0;
    for (const auto &s : strings_)
      total += s.size();
    return total;
  }

private:
  mutable std::shared_mutex mutex_;
  std::deque<std::string> strings_;
  std::unordered_map<std::string_view, Id> index_;

  void rebuildIndex() {
    index_.clear();
    for (size_t i = 0; i < strings_.size(); ++i)
      index_.emplace(std::string_view(strings_[i]), static_cast<Id>(i));
  }
};

} // namespace vycor
