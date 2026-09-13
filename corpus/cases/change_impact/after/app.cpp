#include "ops.h"
#include <stdexcept>

int helper(int x) { return x + 1; }

int process(int x) {
  try {
    return helper(x);
  } catch (const std::exception &) {
    return -1;
  }
}

int fresh(int x) { return helper(x) * 2; }

int main() { return process(1) + fresh(2); }
