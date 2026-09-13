#include "ops.h"
#include <stdexcept>


// Whitespace only: every function below moves down a few lines and is
// re-indented. The diff against the before index must be empty.

int helper(int x) {
    return x + 1;
}

int process(int x) {
    return helper(x);
}

int main() {
    return process(1);
}
