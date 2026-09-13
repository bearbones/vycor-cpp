#include "ops.h"

int retired(int x) { return x * 2; }

int caller_of_retired() { return retired(1); }
