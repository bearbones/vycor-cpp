#include "ops.h"
#include <stdexcept>

int helper(int x) { return x + 1; }

int process(int x) {
  return helper(x);
}

int main() { return process(1); }
