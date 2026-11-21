#include "core/core.h"
#include "shared/shared.h"
#include "util/util.h"

int tool_run(int x) {
  return util_trim(x) + core_sum(x, 1) + core_version() + shared_helper(x);
}
