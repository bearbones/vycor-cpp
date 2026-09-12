#include "f.h"
int g() { return f(1); }
int h() { return f(2.0); }
int main() { return g() + h() + f(3); }
