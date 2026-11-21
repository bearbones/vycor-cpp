#include "registry.h"
#include <cstdio>
#include <thread>

std::mutex logMutex;
std::mutex statsMutex;
static std::string pending;

void logLine(const std::string &line) {
  std::lock_guard<std::mutex> lock(logMutex);
  pending += line;
}

void flushLog() {
  std::lock_guard<std::mutex> lock(logMutex);
  std::printf("%d entries: %s\n", registryCount(), pending.c_str());
  pending.clear();
}

void pollStats() {
  std::lock_guard<std::mutex> stats(statsMutex);
  std::unique_lock<std::mutex> lock(logMutex, std::try_to_lock);
}

int main() {
  std::thread a([] { registerItem("a"); });
  std::thread b([] { flushLog(); });
  snapshotBoth();
  pollStats();
  reenter(2);
  a.join();
  b.join();
  return 0;
}
