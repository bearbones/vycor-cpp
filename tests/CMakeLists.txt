# Catch2 v3.10+ resolution.
#
# Tries three sources in priority order so organizations with hermetic builds
# can override however suits their toolchain:
#
#   1. A pre-existing Catch2::Catch2WithMain target. A parent project, internal
#      package manager, or CMake toolchain file may create the imported target
#      before this directory is added — we honor it untouched.
#
#   2. find_package(Catch2 3.10 CONFIG). Covers system installs, vcpkg, Conan,
#      Spack, and any layout that ships Catch2's CMake config files. Override
#      the search by setting CMAKE_PREFIX_PATH or Catch2_DIR.
#
#   3. Vendored submodule at extern/Catch2. Default for hands-off builds.
#      Replace the URL in .gitmodules to point at an internal mirror or fork.
#
# We deliberately do NOT use FetchContent: hermetic builds should not require
# network access at configure time, and the source of Catch2 should be a
# decision the caller's environment makes — not ours.

set(_catch2_origin "")

if(NOT TARGET Catch2::Catch2WithMain)
  find_package(Catch2 3.10 QUIET CONFIG)
  if(Catch2_FOUND)
    set(_catch2_origin "package (${Catch2_DIR})")
  endif()
endif()

if(NOT TARGET Catch2::Catch2WithMain)
  set(_catch2_submodule "${CMAKE_SOURCE_DIR}/extern/Catch2")
  if(EXISTS "${_catch2_submodule}/CMakeLists.txt")
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS ON CACHE BOOL "" FORCE)
    # Explicit binary dir required because the source lives outside this
    # directory (extern/ is a sibling of tests/, not a child).
    add_subdirectory("${_catch2_submodule}" "${CMAKE_BINARY_DIR}/extern/Catch2"
                     EXCLUDE_FROM_ALL)
    list(APPEND CMAKE_MODULE_PATH "${_catch2_submodule}/extras")
    set(_catch2_origin "submodule (extern/Catch2)")
  endif()
endif()

if(NOT TARGET Catch2::Catch2WithMain)
  message(FATAL_ERROR
    "Catch2 (>= 3.10) not found. Provide it via one of:\n"
    "  - find_package: install Catch2 v3.10+ and set CMAKE_PREFIX_PATH if\n"
    "    it lives outside the default search paths\n"
    "  - imported target: a parent project or toolchain may pre-define\n"
    "    Catch2::Catch2WithMain before this directory is configured\n"
    "  - vendored submodule: git submodule update --init extern/Catch2\n"
    "Or disable tests entirely with -DVYCOR_BUILD_TESTS=OFF.")
endif()

message(STATUS "Using Catch2 from ${_catch2_origin}")

add_executable(vycor_tests
  test_matcher_engine.cpp
  test_rules_parser.cpp
  test_transforms.cpp
  test_adl_ctad.cpp
  test_coverage_diag.cpp
  test_dead_code.cpp
  test_deferred_edges.cpp
  test_precision_identity.cpp
  test_disambiguation.cpp
  test_control_flow.cpp
  test_concurrency_index.cpp
  test_channel_index.cpp
  test_mcp.cpp
  test_deep_chains.cpp
  test_snapshot.cpp
  test_worker_pool.cpp
)

target_link_libraries(vycor_tests PRIVATE
  vycor_lib
  Catch2::Catch2WithMain
)

target_compile_definitions(vycor_tests PRIVATE
  PROJECT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

include(Catch)
catch_discover_tests(vycor_tests)
