#!/usr/bin/env bash
# Generates a compile_commands.json for the deep_chains fixture, rooted
# at the current directory so `megascope` (`--build-path`) can find it.
#
# Usage: run from inside examples/deep_chains/:
#   ./gen_compile_commands.sh
#
# Produces ./compile_commands.json with absolute paths.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"

cxx="${CXX:-clang++}"
std="-std=c++17"

srcs=(
  main.cpp
  pipeline.cpp
  stage1_ingest.cpp
  stage2_parse.cpp
  stage3_transform.cpp
  stage4_dispatch.cpp
  stage5_sink.cpp
  plugins.cpp
  workers.cpp
  tokenizer.cpp
  scheduler.cpp
  callbacks.cpp
  async_workers.cpp
  lambda_callbacks.cpp
)

{
  echo "["
  first=1
  for s in "${srcs[@]}"; do
    if [[ $first -eq 1 ]]; then first=0; else echo ","; fi
    cat <<JSON
  {
    "directory": "${HERE}",
    "file": "${HERE}/${s}",
    "arguments": ["${cxx}", "${std}", "-I", "${HERE}", "-c", "${HERE}/${s}"]
  }
JSON
  done
  echo "]"
} > "${HERE}/compile_commands.json"

echo "Wrote ${HERE}/compile_commands.json (${#srcs[@]} entries)"
