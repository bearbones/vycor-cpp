add_library(vycor_lib
  anneal/GlobalIndex.cpp
  anneal/Indexer.cpp
  anneal/Analyzer.cpp
  anneal/DeadCodeAnalyzer.cpp
  morph/MatcherEngine.cpp
  morph/TemplateEngine.cpp
  morph/RulesParser.cpp
  morph/TransformPipeline.cpp
  compat/PchCache.cpp
  callgraph/CallGraph.cpp
  callgraph/CallGraphBuilder.cpp
  callgraph/CollapseFilter.cpp
  callgraph/ControlFlowIndex.cpp
  callgraph/ControlFlowContextVisitor.cpp
  callgraph/ChannelIndex.cpp
  callgraph/ControlFlowOracle.cpp
  callgraph/Snapshot.cpp
  callgraph/WorkerPool.cpp
  mcp/McpProtocol.cpp
  mcp/McpServer.cpp
  mcp/McpTools.cpp
)

target_include_directories(vycor_lib PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated
)

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

# Embed the Clang resource directory so the tool can find built-in headers
# (stdarg.h, etc.) when consuming compilation databases from other toolchains.
if(LLVM_FOUND)
  execute_process(
    COMMAND "${LLVM_TOOLS_BINARY_DIR}/clang" -print-resource-dir
    OUTPUT_VARIABLE _VYCOR_RESOURCE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(_VYCOR_RESOURCE_DIR)
    target_compile_definitions(vycor_lib PRIVATE
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
    target_compile_definitions(vycor_lib PRIVATE
      "VYCOR_DEFAULT_SYSROOT=\"${_VYCOR_DEFAULT_SYSROOT}\"")
  endif()
endif()

add_executable(vycor-cpp main.cpp)
target_link_libraries(vycor-cpp PRIVATE vycor_lib)

# Embed the default --clang path for PCH compilation into the executable.
target_compile_definitions(vycor-cpp PRIVATE
  "VYCOR_DEFAULT_CLANG=\"${VYCOR_DEFAULT_CLANG}\"")

# Release packaging: `cmake --install build --prefix <dir>` stages a single
# self-contained bin/ dir that release.yml tars up. No CPack -- one binary
# doesn't need it (see docs/toolchain-portability.md for why this project
# keeps packaging minimal).
include(GNUInstallDirs)
install(TARGETS vycor-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
