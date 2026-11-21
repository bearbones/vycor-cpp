// Compiled into both executables: every target that lists it owns its
// symbols.
#include "shared/shared.h"

static int shared_detail(int x) { return x * 2; }

int shared_helper(int x) { return shared_detail(x); }
