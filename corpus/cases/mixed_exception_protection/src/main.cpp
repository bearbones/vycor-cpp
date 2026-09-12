#include "lib.h"
#include <exception>
void safe_caller() {
  try {
    risky();
  } catch (const std::exception &) {
  }
}
void unsafe_caller() { risky(); }
int main() {
  safe_caller();
  unsafe_caller();
  try {
    guarded();
  } catch (const std::exception &) {
  }
  return 0;
}
