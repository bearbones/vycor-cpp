#include "f.h"
int f(int x) { return x; }
int f(double x) { return static_cast<int>(x); }
