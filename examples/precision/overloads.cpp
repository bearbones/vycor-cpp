#include "overloads.hpp"

namespace precision {

int process(const char *raw) { return raw ? 1 : 0; }
int process(double scaled) { return scaled > 0.5 ? 1 : 0; }

int fromCString() { return process("x"); }
int fromDouble() { return process(0.75); }

} // namespace precision
