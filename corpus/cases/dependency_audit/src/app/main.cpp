#include "core/inline.h"
#include "shared/shared.h"

static int measure(const Shape &s) { return s.area(); }

int app_run(const Shape &s) { return measure(s) + twice(4) + shared_helper(1); }

int main() { return twice(2); }
