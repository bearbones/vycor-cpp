add_library(vycor_lib
  check/GlobalIndex.cpp
  check/Indexer.cpp
  check/Analyzer.cpp
  check/Checkpoint.cpp
  check/CheckSet.cpp
  check/DeadCodeAnalyzer.cpp
  check/Report.cpp
  morph/MatcherEngine.cpp
  morph/TemplateEngine.cpp
  morph/RulesParser.cpp
  morph/TransformPipeline.cpp
  compat/PchCache.cpp
  callgraph/AtomicFile.cpp
  callgraph/CallGraph.cpp
  callgraph/CallGraphBuilder.cpp
  callgraph/CollapseFilter.cpp
  callgraph/ControlFlowIndex.cpp
  callgraph/ControlFlowContextVisitor.cpp
  callgraph/ChannelIndex.cpp
  callgraph/ControlFlowOracle.cpp
  callgraph/InputFingerprint.cpp
  callgraph/MegascopeBridge.cpp
  callgraph/PathSearch.cpp
  callgraph/Snapshot.cpp
  callgraph/WorkerPool.cpp
  callgraph/Interrupt.cpp
  callgraph/CrashGuard.cpp
  callgraph/RetainUntilExit.cpp
  query/Serialize.cpp
  query/Identity.cpp
  query/GraphTools.cpp
  query/ExceptionTools.cpp
  query/LockTools.cpp
  query/DeadCodeTools.cpp
  query/ChannelTools.cpp
  query/Registry.cpp
  query/Limits.cpp
  query/ImpactTools.cpp
  impact/SemanticDiff.cpp
  impact/ImpactSearch.cpp
  impact/PatchMapping.cpp
  cli/BakeConfig.cpp
  cli/DiffVerb.cpp
  cli/MegascopeCli.cpp
  cli/SourceSelection.cpp
  ext/Extensions.cpp
  ext/OrgConfig.cpp
)

set_target_properties(vycor_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(vycor_lib PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated
)

option(VYCOR_USE_LIBMEGASCOPE "Link against libmegascope C FFI if found" ON)
set(MEGASCOPE_ROOT "${CMAKE_SOURCE_DIR}/../megascope" CACHE PATH "Path to the megascope Rust core repository or release prefix")
if(VYCOR_USE_LIBMEGASCOPE AND NOT _vycor_sanitize)
  find_path(MEGASCOPE_INCLUDE_DIR
    NAMES megascope.h
    HINTS "${MEGASCOPE_ROOT}/include"
    NO_DEFAULT_PATH
  )
  find_library(MEGASCOPE_STATIC_LIB
    NAMES libmegascope.a megascope
    HINTS
      "${MEGASCOPE_ROOT}/lib"
      "${MEGASCOPE_ROOT}/target/release"
      "${MEGASCOPE_ROOT}/target/debug"
    NO_DEFAULT_PATH
  )
endif()
if(MEGASCOPE_INCLUDE_DIR AND MEGASCOPE_STATIC_LIB)
  message(STATUS "Found libmegascope: ${MEGASCOPE_STATIC_LIB} (include: ${MEGASCOPE_INCLUDE_DIR})")
  target_include_directories(vycor_lib PUBLIC "${MEGASCOPE_INCLUDE_DIR}")
  target_compile_definitions(vycor_lib PUBLIC VYCOR_HAS_LIBMEGASCOPE=1)
  target_link_libraries(vycor_lib PUBLIC "${MEGASCOPE_STATIC_LIB}" dl pthread m)
else()
  message(STATUS "libmegascope not linked (MEGASCOPE_ROOT=${MEGASCOPE_ROOT}); building with JSON shard export only")
endif()

target_compile_options(vycor_lib PRIVATE ${VYCOR_WARNING_FLAGS})

if(LLVM_LINK_LLVM_DYLIB AND CLANG_LINK_CLANG_DYLIB AND TARGET clang-cpp AND TARGET LLVM)
  target_link_libraries(vycor_lib PUBLIC
    clang-cpp
    LLVM
  )
else()
  target_link_libraries(vycor_lib PUBLIC
    clangTooling
    clangDynamicASTMatchers
    clangASTMatchers
    clangAST
    clangIndex
    clangBasic
    clangDriver
    clangFrontend
    clangSema
    clangSerialization
    clangRewrite
    clangToolingCore
    LLVMSupport
  )
endif()

# Embed the Clang resource directory so the tool can find built-in headers
# (stdarg.h, etc.) when consuming compilation databases from other toolchains.
if(LLVM_FOUND)
  execute_process(
    COMMAND "${LLVM_TOOLS_BINARY_DIR}/clang" -print-resource-dir
    OUTPUT_VARIABLE _VYCOR_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_VYCOR_RESOURCE_DIR)
    # PUBLIC, not PRIVATE: getResourceDirAdjuster() is an inline function in
    # ToolAdjusters.h whose body is #ifdef'd on this macro. Any consumer TU
    # (main.cpp, a test) compiled without it emits a no-op weak definition
    # that can win symbol resolution over the lib's real one — silently
    # dropping -resource-dir injection binary-wide (ODR).
    target_compile_definitions(vycor_lib PUBLIC
      "VYCOR_CLANG_RESOURCE_DIR=\"${_VYCOR_RESOURCE_DIR}\"")
  endif()
endif()

# On macOS, detect the default SDK sysroot and libc++ include path so that
# ClangTool can find C++ stdlib headers even when the compilation database
# doesn't provide -isysroot (e.g. FixedCompilationDatabase in tests, or
# standalone invocations without a game-engine build path).
if(APPLE)
  execute_process(
    COMMAND xcrun --show-sdk-path
    OUTPUT_VARIABLE _VYCOR_DEFAULT_SYSROOT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_VYCOR_DEFAULT_SYSROOT)
    message(STATUS "Default macOS sysroot: ${_VYCOR_DEFAULT_SYSROOT}")
    # PUBLIC for the same ODR reason as VYCOR_CLANG_RESOURCE_DIR above
    # (getSysrootAdjuster's fallback is #ifdef'd on this macro).
    target_compile_definitions(vycor_lib PUBLIC
      "VYCOR_DEFAULT_SYSROOT=\"${_VYCOR_DEFAULT_SYSROOT}\"")
  endif()
  if(LLVM_FOUND)
    get_filename_component(_VYCOR_DEFAULT_CXX_INCLUDE
      "${LLVM_TOOLS_BINARY_DIR}/../include/c++/v1" ABSOLUTE)
    if(IS_DIRECTORY "${_VYCOR_DEFAULT_CXX_INCLUDE}")
      message(STATUS "Default macOS libc++ include: ${_VYCOR_DEFAULT_CXX_INCLUDE}")
      target_compile_definitions(vycor_lib PUBLIC
        "VYCOR_DEFAULT_CXX_INCLUDE=\"${_VYCOR_DEFAULT_CXX_INCLUDE}\"")
    endif()
  endif()
endif()

# Organization extension slot-in (docs/EXTENDING.md): every .cpp directly
# under <repo>/ext/ is compiled into the executable. Deliberately NOT part
# of vycor_lib — these files typically contain only static registrars
# (VYCOR_REGISTER_CHECK / VYCOR_EXTENSION_SETUP), and the linker
# drops unreferenced members of a static archive, silently discarding the
# registration. Objects listed directly on the target are always linked.
file(GLOB VYCOR_EXT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ext/*.cpp")
if(VYCOR_EXT_SOURCES)
  message(STATUS "Organization extensions: ${VYCOR_EXT_SOURCES}")
endif()

add_executable(vycor-cpp main.cpp ${VYCOR_EXT_SOURCES} ${VYCOR_SANITIZER_SOURCES})
target_link_libraries(vycor-cpp PRIVATE vycor_lib)
target_compile_options(vycor-cpp PRIVATE ${VYCOR_WARNING_FLAGS})

# Embed the default --clang path for PCH compilation into the executable.
target_compile_definitions(vycor-cpp PRIVATE
  "VYCOR_DEFAULT_CLANG=\"${VYCOR_DEFAULT_CLANG}\"")

# Clang frontend plugin (`clang++ -c foo.cpp -fplugin=libvycor_megascope_plugin.so`)
# for zero-reparse per-TU .vycs emission during normal compilation.
add_library(vycor_megascope_plugin MODULE
  plugin/MegascopePlugin.cpp
  ${VYCOR_EXT_SOURCES}
)
target_link_libraries(vycor_megascope_plugin PRIVATE vycor_lib)
target_compile_options(vycor_megascope_plugin PRIVATE ${VYCOR_WARNING_FLAGS})

# Release packaging: `cmake --install build --prefix <dir>` stages a single
# self-contained bin/ dir that release.yml tars up. No CPack -- one binary
# doesn't need it.
include(GNUInstallDirs)
install(TARGETS vycor-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(TARGETS vycor_megascope_plugin LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
