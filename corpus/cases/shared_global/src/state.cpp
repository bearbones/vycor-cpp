#include "state.h"

int raced = 0;
int guarded = 0;
std::mutex guardMutex;
std::atomic<int> hits{0};
int config = 0;
thread_local int scratch = 0;
const int kLimit = 10;

void bump() { ++raced; }
void bumpGuarded() {
  std::lock_guard<std::mutex> lock(guardMutex);
  ++guarded;
}
int readGuarded() {
  std::unique_lock<std::mutex> lock(guardMutex);
  return guarded;
}
