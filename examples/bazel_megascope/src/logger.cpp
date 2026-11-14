#include "core/logger.h"

#include <cstdio>

namespace core {

void writeLogLine(const char *level, const char *message) {
  std::fprintf(stderr, "[%s] %s\n", level, message);
}

} // namespace core
