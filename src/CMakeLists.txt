add_library(vycor_lib
  anneal/GlobalIndex.cpp
  anneal/Indexer.cpp
  anneal/Analyzer.cpp
  anneal/Checkpoint.cpp
  anneal/CheckSet.cpp
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
  ext/Extensions.cpp
  ext/OrgConfig.cpp
)

target_include_directories(vycor_lib PUBLIC
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/generated
)

target_compile_options(vycor_lib PRIVATE ${VYCOR_WARNING_FLAGS})

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
endif()

# Organization extension slot-in (docs/EXTENDING.md): every .cpp directly
# under <repo>/ext/ is compiled into the executable. Deliberately NOT part
# of vycor_lib — these files typically contain only static registrars
# (VYCOR_REGISTER_ANNEAL_CHECK / VYCOR_EXTENSION_SETUP), and the linker
# drops unreferenced members of a static archive, silently discarding the
# registration. Objects listed directly on the target are always linked.
file(GLOB VYCOR_EXT_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/ext/*.cpp")
if(VYCOR_EXT_SOURCES)
  message(STATUS "Organization extensions: ${VYCOR_EXT_SOURCES}")
endif()

add_executable(vycor-cpp main.cpp ${VYCOR_EXT_SOURCES})
target_link_libraries(vycor-cpp PRIVATE vycor_lib)
target_compile_options(vycor-cpp PRIVATE ${VYCOR_WARNING_FLAGS})

# Embed the default --clang path for PCH compilation into the executable.
target_compile_definitions(vycor-cpp PRIVATE
  "VYCOR_DEFAULT_CLANG=\"${VYCOR_DEFAULT_CLANG}\"")

# Release packaging: `cmake --install build --prefix <dir>` stages a single
# self-contained bin/ dir that release.yml tars up. No CPack -- one binary
# doesn't need it (see docs/toolchain-portability.md for why this project
# keeps packaging minimal).
include(GNUInstallDirs)
install(TARGETS vycor-cpp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
