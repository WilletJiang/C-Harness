add_library(ah STATIC
  src/common.c
  src/hash.c
  src/sdk.c

  src/core/completion.c
  src/core/content.c
  src/core/content_codec.c
  src/core/content_expand.c
  src/core/content_store.c
  src/core/context.c
  src/core/context_archive.c
  src/core/context_policy.c
  src/core/context_recall.c
  src/core/engine.c
  src/core/execute.c
  src/core/flow.c
  src/core/flow_schedule.c
  src/core/flow_vm.c
  src/core/inference.c
  src/core/jobs.c
  src/core/mailbox.c
  src/core/observations.c
  src/core/observe.c
  src/core/operations.c
  src/core/recovery.c
  src/core/request.c
  src/core/request_tools.c
  src/core/response.c
  src/core/results.c
  src/core/retention.c
  src/core/retry.c
  src/core/runtime.c
  src/core/session.c
  src/core/step.c
  src/core/store.c
  src/core/task.c
  src/core/task_budget.c
  src/core/task_store.c
  src/core/task_verify.c
  src/core/telemetry.c
  src/core/tool_content.c
  src/core/tool_step.c
  src/core/work_calls.c
  src/core/work_cancel.c
  src/core/work_dispatch.c
  src/core/work_host.c
  src/core/work_join.c
  src/core/work_signal.c
  src/core/work_store.c
  src/core/workspace.c
  src/core/workspace_dispatch.c
  src/core/workspace_grants.c
  src/core/workspace_paths.c
  src/core/write_batch.c

  src/agents/budget.c
  src/agents/definitions.c
  src/agents/join.c
  src/agents/mail.c
  src/agents/model.c
  src/agents/notify.c
  src/agents/scheduler.c
  src/agents/spawn.c
  src/agents/stop.c
  src/agents/store.c
  src/agents/tools.c
  src/agents/usage.c

  src/providers/http.c
  src/providers/responses.c
  src/providers/responses_async.c
  src/providers/responses_http.c
  src/providers/responses_metrics.c
  src/providers/stream.c
  src/providers/stream_items.c

  src/tools/changes.c
  src/tools/check.c
  src/tools/check_registry.c
  src/tools/diagnostics.c
  src/tools/edit.c
  src/tools/exec.c
  src/tools/file.c
  src/tools/file_read.c
  src/tools/find.c
  src/tools/ignore.c
  src/tools/image.c
  src/tools/local.c
  src/tools/schema.c
  src/tools/search.c
  src/tools/search_http.c
  src/tools/search_quota.c
  src/tools/source.c
  src/tools/walk.c

  src/extensions/catalog.c
  src/extensions/catalog_io.c
  src/extensions/catalog_servers.c
  src/extensions/codex.c
  src/extensions/context.c
  src/extensions/extensions.c
  src/extensions/mcp.c
  src/extensions/mcp_config.c
  src/extensions/mcp_content.c
  src/extensions/mcp_http.c
  src/extensions/mcp_stdio.c
  src/extensions/plugins.c
  src/extensions/references.c
  src/extensions/servers.c
  src/extensions/skills.c)
if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set_target_properties(ah PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
endif()
add_dependencies(ah ah_flow_vm)
target_compile_definitions(ah PRIVATE AH_FLOW_INSTALLED="${CMAKE_INSTALL_FULL_BINDIR}/ah-flow-vm")
add_library(ah::ah ALIAS ah)
ah_compile_target(ah)
target_include_directories(ah PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(ah PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(ah PRIVATE ah_json ah_toml CURL::libcurl SQLite::SQLite3 Threads::Threads)
if(APPLE)
  target_link_libraries(ah PRIVATE "-framework CoreFoundation")
elseif(UNIX)
  target_link_libraries(ah PRIVATE m)
endif()
