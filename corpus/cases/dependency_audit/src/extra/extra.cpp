#include "extra/extra.h"

static int extra_helper(int x) { return x + 1; }

int extra_fn(int x) { return extra_helper(x); }
