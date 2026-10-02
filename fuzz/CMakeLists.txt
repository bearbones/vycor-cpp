# libFuzzer targets (VYCOR_FUZZ=ON, clang only; see fuzz/README.md).
# The top-level CMakeLists.txt has already turned on address,undefined and
# coverage instrumentation for every target; these executables add
# libFuzzer's main.

set(VYCOR_FUZZ_TARGETS fuzz_snapshot fuzz_checkpoint fuzz_shard fuzz_batch)

foreach(_target IN LISTS VYCOR_FUZZ_TARGETS)
  add_executable(${_target} ${_target}.cpp ${VYCOR_SANITIZER_SOURCES})
  target_link_libraries(${_target} PRIVATE vycor_lib)
  target_compile_options(${_target} PRIVATE ${VYCOR_WARNING_FLAGS})
  target_link_options(${_target} PRIVATE -fsanitize=fuzzer)
  target_compile_definitions(${_target} PRIVATE
    PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endforeach()

# Seed corpora, generated from the fixtures at the current formats.
add_executable(fuzz_seeds fuzz_seeds.cpp ${VYCOR_SANITIZER_SOURCES})
target_link_libraries(fuzz_seeds PRIVATE vycor_lib)
target_compile_options(fuzz_seeds PRIVATE ${VYCOR_WARNING_FLAGS})
target_compile_definitions(fuzz_seeds PRIVATE
  PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_custom_target(fuzz DEPENDS ${VYCOR_FUZZ_TARGETS} fuzz_seeds)
