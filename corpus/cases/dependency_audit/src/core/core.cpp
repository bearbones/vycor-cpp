#include "core/core.h"

static int clamp(int x) { return x < 0 ? 0 : x; }

int core_scale(int x) { return clamp(x) * 3; }
int core_sum(int a, int b) { return clamp(a) + clamp(b); }
int core_version() { return 3; }
void core_log(const char *msg) { (void)msg; }
int register_plugin(int (*entry)(int)) { return entry ? 1 : 0; }

Shape::~Shape() { core_log("~Shape"); }
int Shape::area() const { return core_version(); }
