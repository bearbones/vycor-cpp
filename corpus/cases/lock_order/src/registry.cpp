#include "registry.h"
#include <vector>

std::mutex registryMutex;
std::recursive_mutex reentrantMutex;
static std::vector<std::string> entries;

void registerItem(const std::string &name) {
  std::lock_guard<std::mutex> lock(registryMutex);
  entries.push_back(name);
  logLine("registered " + name);
}

int registryCount() {
  std::lock_guard<std::mutex> lock(registryMutex);
  return static_cast<int>(entries.size());
}

void snapshotBoth() {
  std::scoped_lock both(registryMutex, logMutex);
}

void reenter(int depth) {
  std::lock_guard<std::recursive_mutex> lock(reentrantMutex);
  if (depth > 0)
    reenter(depth - 1);
}
