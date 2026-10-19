#pragma once

namespace core {

void writeLogLine(const char *level, const char *message);
bool isVerboseEnabled();

inline void logInfo(const char *message) {
  if (isVerboseEnabled()) {
    writeLogLine("INFO", message);
  }
}

} // namespace core
