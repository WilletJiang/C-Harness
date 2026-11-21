cmake_minimum_required(VERSION 3.20)
project(ah VERSION 0.11.0 LANGUAGES C)

set(AH_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(AH_TOP_LEVEL ON)
  include(CTest)
endif()
set(AH_TESTS_ENABLED OFF)
if(AH_TOP_LEVEL AND BUILD_TESTING)
  set(AH_TESTS_ENABLED ON)
endif()
option(AH_BUILD_CLI "Build the headless CLI/RPC executable" ${AH_TOP_LEVEL})
option(AH_BUILD_EXAMPLES "Build SDK examples" ${AH_TOP_LEVEL})
if(AH_TESTS_ENABLED AND (NOT AH_BUILD_CLI OR NOT AH_BUILD_EXAMPLES))
  message(FATAL_ERROR "Contract tests exercise CLI/examples; set BUILD_TESTING=OFF for a library-only build")
endif()
if(AH_TESTS_ENABLED)
  enable_testing()
endif()
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
option(AH_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(AH_FUZZ "Build libFuzzer targets (Clang)" OFF)
option(AH_BENCHMARK "Build local orchestration and file benchmarks" OFF)
option(AH_LIVE_EVAL "Build explicitly authorized live evaluation drivers" OFF)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR})
find_package(CURL REQUIRED)
find_package(SQLite3 3.37 REQUIRED)
find_package(Threads REQUIRED)
if(AH_TESTS_ENABLED)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()
if(AH_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

function(ah_compile_target target)
  target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L _GNU_SOURCE=1 _DARWIN_C_SOURCE=1)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

# Vendor code has its own warning policy; it is not compiled with our -Werror.
add_library(ah_json STATIC vendor/yyjson/yyjson.c)
target_include_directories(ah_json PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/yyjson>)
set_target_properties(ah_json PROPERTIES POSITION_INDEPENDENT_CODE ON)
add_library(ah_toml STATIC vendor/tomlc17/tomlc17.c)
target_include_directories(ah_toml PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/tomlc17>)
set_target_properties(ah_toml PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Keep the JavaScript engine outside the SDK address space.
add_executable(ah_flow_vm flow/main.c vendor/quickjs/quickjs.c vendor/quickjs/cutils.c
  vendor/quickjs/libunicode.c vendor/quickjs/libregexp.c vendor/quickjs/dtoa.c)
set_target_properties(ah_flow_vm PROPERTIES OUTPUT_NAME ah-flow-vm C_EXTENSIONS ON)
target_include_directories(ah_flow_vm PRIVATE vendor/quickjs)
target_compile_definitions(ah_flow_vm PRIVATE _GNU_SOURCE CONFIG_VERSION="2026-06-04")
# QuickJS's large interpreter frame under -O0 + sanitizer instrumentation
# consumes its script stack limit during built-in calls. Keep the VM optimized.
target_compile_options(ah_flow_vm PRIVATE -O2)
target_link_libraries(ah_flow_vm PRIVATE m Threads::Threads)
install(TARGETS ah_flow_vm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_library(ah STATIC
  src/core/flow.c src/core/flow_vm.c src/core/flow_schedule.c src/core/write_batch.c
  src/common.c src/hash.c src/sdk.c
  src/core/content.c src/core/content_codec.c src/core/content_store.c src/core/content_expand.c src/core/tool_content.c src/core/retention.c
  src/core/context_policy.c src/core/context_archive.c src/core/context_recall.c
  src/core/context.c src/core/engine.c src/core/workspace.c src/core/workspace_paths.c src/core/workspace_grants.c src/core/workspace_dispatch.c src/core/execute.c src/core/completion.c src/core/mailbox.c src/core/observations.c src/core/retry.c
  src/core/observe.c src/core/work_store.c src/core/work_signal.c src/core/work_dispatch.c
  src/core/work_calls.c src/core/work_join.c src/core/work_host.c src/core/work_cancel.c
  src/core/operations.c src/core/jobs.c src/core/tool_step.c src/core/recovery.c src/core/response.c src/core/request.c
  src/core/results.c src/core/runtime.c src/core/step.c src/core/inference.c src/core/session.c src/core/store.c src/core/telemetry.c
  src/core/task_budget.c src/core/task_store.c src/core/task_verify.c src/core/task.c
  src/agents/scheduler.c src/agents/model.c src/agents/store.c src/agents/mail.c
  src/agents/notify.c src/agents/budget.c src/agents/stop.c src/agents/spawn.c
  src/agents/join.c src/agents/tools.c src/agents/definitions.c src/agents/usage.c
  src/extensions/extensions.c src/extensions/context.c src/extensions/references.c src/extensions/servers.c
  src/extensions/catalog.c src/extensions/catalog_io.c src/extensions/catalog_servers.c
  src/extensions/codex.c src/extensions/skills.c src/extensions/plugins.c
  src/extensions/mcp.c src/extensions/mcp_content.c src/extensions/mcp_config.c src/extensions/mcp_stdio.c src/extensions/mcp_http.c
  src/providers/responses.c src/providers/responses_http.c src/providers/responses_async.c src/providers/responses_metrics.c
  src/providers/stream.c src/providers/stream_items.c
  src/tools/source.c src/tools/walk.c src/tools/ignore.c src/tools/find.c src/tools/changes.c src/tools/edit.c
  src/tools/check.c src/tools/check_registry.c src/tools/diagnostics.c
  src/tools/exec.c src/tools/file.c src/tools/file_read.c src/tools/image.c src/tools/local.c src/tools/schema.c)
add_dependencies(ah ah_flow_vm)
target_compile_definitions(ah PRIVATE AH_FLOW_INSTALLED="${CMAKE_INSTALL_FULL_BINDIR}/ah-flow-vm")
add_library(ah::ah ALIAS ah)
ah_compile_target(ah)
target_include_directories(ah PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(ah PRIVATE src)
target_link_libraries(ah PRIVATE ah_json ah_toml CURL::libcurl SQLite::SQLite3 Threads::Threads)
if(APPLE)
  target_link_libraries(ah PRIVATE "-framework CoreFoundation")
elseif(UNIX)
  target_link_libraries(ah PRIVATE m)
endif()

if(AH_BUILD_CLI)
  add_executable(ah_cli
    src/cli/main.c src/cli/output.c src/cli/rpc.c src/cli/credential.c src/cli/signals.c
    src/cli/options.c src/cli/extensions.c src/cli/content.c src/cli/policies.c)
  set_target_properties(ah_cli PROPERTIES OUTPUT_NAME ah)
  ah_compile_target(ah_cli)
  target_include_directories(ah_cli PRIVATE src)
  target_link_libraries(ah_cli PRIVATE ah ah_json Threads::Threads)
endif()
if(AH_BUILD_EXAMPLES)
  add_executable(ah_embed examples/embed.c)
  target_link_libraries(ah_embed PRIVATE ah)
  add_executable(ah_host examples/host.c)
  target_link_libraries(ah_host PRIVATE ah)
  add_executable(ah_durable examples/durable.c)
  target_link_libraries(ah_durable PRIVATE ah)
endif()

if(AH_LIVE_EVAL OR AH_TESTS_ENABLED)
  add_subdirectory(evals)
endif()
if(AH_TESTS_ENABLED OR AH_FUZZ)
  add_subdirectory(tests)
endif()
if(AH_BENCHMARK)
  add_subdirectory(benchmarks)
endif()

install(TARGETS ah ah_json ah_toml EXPORT ahTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(AH_BUILD_CLI)
  install(TARGETS ah_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT ahTargets NAMESPACE ah:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ah)
configure_package_config_file(cmake/ahConfig.cmake.in ${CMAKE_CURRENT_BINARY_DIR}/ahConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ah)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/ahConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ahConfig.cmake ${CMAKE_CURRENT_BINARY_DIR}/ahConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ah)
