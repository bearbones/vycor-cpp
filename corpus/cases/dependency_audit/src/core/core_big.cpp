// The bulk of `core`: a chain nothing outside the library's own entry
// point reaches, so a consumer of core_scale alone needs none of it.
#include "core/core.h"

static int big_1(int x) { return core_sum(x, 1); }
static int big_2(int x) { return big_1(x) + 2; }
static int big_3(int x) { return big_2(x) + 3; }
static int big_4(int x) { return big_3(x) + 4; }
static int big_5(int x) { return big_4(x) + 5; }
static int big_6(int x) { return big_5(x) + 6; }
static int big_7(int x) { return big_6(x) + 7; }
static int big_8(int x) { return big_7(x) + 8; }
static int big_9(int x) { return big_8(x) + 9; }
static int big_10(int x) { return big_9(x) + 10; }
static int big_11(int x) { return big_10(x) + 11; }
static int big_12(int x) { return big_11(x) + 12; }
static int big_13(int x) { return big_12(x) + 13; }
static int big_14(int x) { return big_13(x) + 14; }
static int big_15(int x) { return big_14(x) + 15; }
static int big_16(int x) { return big_15(x) + 16; }

int big_entry(int x) { return big_16(x); }
