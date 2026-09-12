#include "lib.h"
#include <stdexcept>
void risky() { throw std::runtime_error("risky"); }
void guarded() { throw std::runtime_error("guarded"); }
