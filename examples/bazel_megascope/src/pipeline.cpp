#include "engine/pipeline.h"
#include "core/logger.h"

#include <exception>
#include <mutex>

namespace engine {

namespace {
std::mutex gPipelineMutex;
} // namespace

int executePipeline(int input) {
  std::lock_guard<std::mutex> lock(gPipelineMutex);
  core::logInfo("starting pipeline execution");
  try {
    return runWorkerStep(input);
  } catch (const std::exception &ex) {
    core::writeLogLine("ERROR", ex.what());
    return -1;
  }
}

} // namespace engine
