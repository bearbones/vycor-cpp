#include "engine/pipeline.h"
#include "core/logger.h"

#include <stdexcept>

namespace engine {

int runWorkerStep(int payload) {
  core::logInfo("running worker step");
  if (payload < 0) {
    throw std::runtime_error("negative payload");
  }
  return payload * 2;
}

} // namespace engine
