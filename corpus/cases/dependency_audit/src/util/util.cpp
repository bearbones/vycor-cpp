#include "util/util.h"

#include "core/core.h"
#include "core/inline.h"

int util_format(int x) { return core_sum(x, x) + twice(x); }
int util_trim(int x) { return util_format(x) - 1; }
