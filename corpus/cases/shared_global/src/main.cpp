#include "state.h"
#include <thread>

static std::once_flag onceFlag;
static int initialized = 0;   // written once under call_once

static void writer() {
  bump();
  bumpGuarded();
  hits.fetch_add(1);
  scratch = config + kLimit;
  std::call_once(onceFlag, [] { initialized = 1; });
}

static void reader() {
  int seen = raced + readGuarded() + initialized;
  (void)seen;
}

int main() {
  config = 3;
  std::thread a(writer);
  std::thread b(reader);
  a.join();
  b.join();
  return raced;
}
