#include "api.h"

int lonely() { return 1; }        // tier 1: no header, no other TU
int lonelyVar = 3;                // tier 1, a variable
int shared() { return lonely() + lonelyVar; }  // user.cpp calls it
int headerOnly() { return 2; }    // tier 2: header, but no other TU
int sharedVar = 4;                // user.cpp reads it
int forTests() { return 5; }      // only tests/lib_test.cpp calls it

static int already() { return 0; }
namespace { int anon() { return already(); } }
extern "C" int cfunc() { return anon(); }
__attribute__((used)) int kept() { return 0; }
struct S { void method(); };
void S::method() {}
template <typename T> T pick(T t) { return t; }
template <> int pick<int>(int t) { return t + 1; }
inline int inl() { return 0; }
int plugin_entry() { return 0; }  // named with --entry-point

int main() { return shared() + inl(); }
