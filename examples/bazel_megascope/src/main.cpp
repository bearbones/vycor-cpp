#include "core/logger.h"
#include "engine/pipeline.h"

int main() {
  core::logInfo("application startup");
  return engine::executePipeline(21) == 42 ? 0 : 1;
}
