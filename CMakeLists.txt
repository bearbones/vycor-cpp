cmake_minimum_required(VERSION 3.20)

# vycor-cpp's own SemVer, independent of the LLVM major it's built against.
# See COMPATIBILITY.md "Tagging and releases" for the tag grammar that
# derives from this file.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" VYCOR_VERSION_RAW LIMIT_COUNT 1)
string(STRIP "${VYCOR_VERSION_RAW}" VYCOR_VERSION_RAW)

project(vycor-cpp VERSION ${VYCOR_VERSION_RAW} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/include/vycor/Version.h.in"
  "${CMAKE_BINARY_DIR}/generated/vycor/Version.h"
  @ONLY)

# --- Organization override hook ---
# Forks can add cmake/org-overrides.cmake to customize dependency resolution,
# set CMAKE_PREFIX_PATH, define wrapper targets, etc. This file is gitignored
# upstream and committed in org forks. See docs/toolchain-portability.md.
include(cmake/org-overrides.cmake OPTIONAL)

# --- Options ---
option(VYCOR_USE_SUBMODULE "Fall back to extern/llvm-project submodule if no system LLVM" ON)
set(VYCOR_DEFAULT_CLANG "clang++" CACHE STRING
    "Default clang++ binary for PCH compilation (--clang default)")

# --- LLVM/Clang: prefer system packages, fall back to submodule ---
# Supported LLVM versions, checked in order (newest first).
# To add a new version, append it to this list.
set(VYCOR_SUPPORTED_LLVM_VERSIONS 21 20 18)

set(LLVM_SUBMODULE_DIR "${CMAKE_SOURCE_DIR}/extern/llvm-project/llvm")

# Try each supported version by probing well-known install paths.
# LLVM's version-compat file requires major.minor to match exactly, so we
# search without a version constraint and verify the major version ourselves.
foreach(_ver IN LISTS VYCOR_SUPPORTED_LLVM_VERSIONS)
  find_package(LLVM CONFIG QUIET
    PATHS "/usr/lib/llvm-${_ver}/lib/cmake/llvm"
    NO_DEFAULT_PATH)
  if(LLVM_FOUND AND LLVM_VERSION_MAJOR STREQUAL "${_ver}")
    find_package(Clang CONFIG QUIET
      HINTS "${LLVM_INSTALL_PREFIX}/lib/cmake/clang"
            "${LLVM_DIR}/../clang")
    if(Clang_FOUND)
      break()
    endif()
  endif()
  # Reset state so the next iteration starts clean.
  unset(LLVM_FOUND)
  unset(LLVM_DIR CACHE)
  unset(LLVM_INSTALL_PREFIX)
  unset(Clang_FOUND)
  unset(Clang_DIR CACHE)
endforeach()

# If the well-known paths didn't work, try CMAKE_PREFIX_PATH / system paths.
if(NOT LLVM_FOUND)
  find_package(LLVM CONFIG QUIET)
  if(LLVM_FOUND)
    list(FIND VYCOR_SUPPORTED_LLVM_VERSIONS "${LLVM_VERSION_MAJOR}" _idx)
    if(_idx GREATER_EQUAL 0)
      find_package(Clang CONFIG QUIET
        HINTS "${LLVM_INSTALL_PREFIX}/lib/cmake/clang"
              "${LLVM_DIR}/../clang")
    else()
      message(WARNING "Found LLVM ${LLVM_PACKAGE_VERSION}, but only versions "
              "${VYCOR_SUPPORTED_LLVM_VERSIONS} are supported.")
      unset(LLVM_FOUND)
    endif()
  endif()
endif()

if(LLVM_FOUND AND Clang_FOUND)
  message(STATUS "Using system LLVM ${LLVM_PACKAGE_VERSION}")
  message(STATUS "Using system Clang from ${Clang_DIR}")
  include_directories(SYSTEM ${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})
elseif(VYCOR_USE_SUBMODULE AND EXISTS "${LLVM_SUBMODULE_DIR}/CMakeLists.txt")
  message(STATUS "Using LLVM/Clang from submodule")
  set(LLVM_ENABLE_PROJECTS "clang" CACHE STRING "" FORCE)
  set(LLVM_TARGETS_TO_BUILD "X86;AArch64" CACHE STRING "" FORCE)
  set(LLVM_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLVM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLVM_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
  set(LLVM_INCLUDE_BENCHMARKS OFF CACHE BOOL "" FORCE)
  set(LLVM_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
  add_subdirectory(${LLVM_SUBMODULE_DIR} EXCLUDE_FROM_ALL)
  include_directories(
    ${CMAKE_SOURCE_DIR}/extern/llvm-project/llvm/include
    ${CMAKE_BINARY_DIR}/extern/llvm-project/llvm/include
    ${CMAKE_SOURCE_DIR}/extern/llvm-project/clang/include
    ${CMAKE_BINARY_DIR}/extern/llvm-project/llvm/tools/clang/include
  )
else()
  string(REPLACE ";" ", " _ver_list "${VYCOR_SUPPORTED_LLVM_VERSIONS}")
  message(FATAL_ERROR
    "LLVM/Clang not found. Install one of the supported versions "
    "(${_ver_list}), e.g. llvm-18-dev and libclang-18-dev, "
    "or initialize the submodule: git submodule update --init")
endif()

# Match LLVM's RTTI setting (off by default) to avoid linker errors
if(NOT LLVM_ENABLE_RTTI)
  if(MSVC)
    string(APPEND CMAKE_CXX_FLAGS " /GR-")
  else()
    string(APPEND CMAKE_CXX_FLAGS " -fno-rtti")
  endif()
endif()

# Warnings for this project's own sources (applied per-target in
# src/CMakeLists.txt and tests/CMakeLists.txt — never to extern/
# submodules). VYCOR_WERROR promotes them to errors; default OFF so a new
# warning in a future compiler release doesn't break downstream users'
# builds, but CI turns it ON — warnings fail the merge gate, not the user.
option(VYCOR_WERROR "Treat compiler warnings as errors (CI default)" OFF)
if(MSVC)
  set(VYCOR_WARNING_FLAGS /W4)
  if(VYCOR_WERROR)
    list(APPEND VYCOR_WARNING_FLAGS /WX)
  endif()
else()
  # -Wno-missing-field-initializers: aggregate init that relies on default
  # member initializers for trailing fields ({caller, callee, kind} on
  # CallGraphEdge etc.) is idiomatic here, not an accident.
  set(VYCOR_WARNING_FLAGS -Wall -Wextra -Wno-missing-field-initializers)
  if(VYCOR_WERROR)
    list(APPEND VYCOR_WARNING_FLAGS -Werror)
  endif()
endif()

add_subdirectory(src)

option(VYCOR_BUILD_TESTS "Build tests (requires network for Catch2)" ON)
if(VYCOR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
