add_library(ah STATIC
  common.c
  hash.c
  sdk.c

  core/completion.c
  core/content.c
  core/content_codec.c
  core/content_expand.c
  core/content_store.c
  core/context.c
  core/context_archive.c
  core/context_policy.c
  core/context_recall.c
  core/engine.c
  core/execute.c
  core/flow.c
  core/flow_schedule.c
  core/flow_vm.c
  core/inference.c
  core/jobs.c
  core/mailbox.c
  core/observations.c
  core/observe.c
  core/operations.c
  core/recovery.c
  core/request.c
  core/response.c
  core/results.c
  core/retention.c
  core/retry.c
  core/runtime.c
  core/session.c
  core/step.c
  core/store.c
  core/task.c
  core/task_budget.c
  core/task_store.c
  core/task_verify.c
  core/telemetry.c
  core/tool_content.c
  core/tool_step.c
  core/work_calls.c
  core/work_cancel.c
  core/work_dispatch.c
  core/work_host.c
  core/work_join.c
  core/work_signal.c
  core/work_store.c
  core/workspace.c
  core/workspace_dispatch.c
  core/workspace_grants.c
  core/workspace_paths.c
  core/write_batch.c

  agents/budget.c
  agents/definitions.c
  agents/join.c
  agents/mail.c
  agents/model.c
  agents/notify.c
  agents/scheduler.c
  agents/spawn.c
  agents/stop.c
  agents/store.c
  agents/tools.c
  agents/usage.c

  providers/http.c
  providers/responses.c
  providers/responses_async.c
  providers/responses_http.c
  providers/responses_metrics.c
  providers/stream.c
  providers/stream_items.c

  tools/changes.c
  tools/check.c
  tools/check_registry.c
  tools/diagnostics.c
  tools/edit.c
  tools/exec.c
  tools/file.c
  tools/file_read.c
  tools/find.c
  tools/ignore.c
  tools/image.c
  tools/local.c
  tools/schema.c
  tools/search.c
  tools/search_http.c
  tools/search_quota.c
  tools/source.c
  tools/walk.c

  extensions/catalog.c
  extensions/catalog_io.c
  extensions/catalog_servers.c
  extensions/codex.c
  extensions/context.c
  extensions/extensions.c
  extensions/mcp.c
  extensions/mcp_config.c
  extensions/mcp_content.c
  extensions/mcp_http.c
  extensions/mcp_stdio.c
  extensions/plugins.c
  extensions/references.c
  extensions/servers.c
  extensions/skills.c)
if(NOT DEFINED CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
  set_target_properties(ah PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")
endif()
add_dependencies(ah ah_flow_vm)
target_compile_definitions(ah PRIVATE AH_FLOW_INSTALLED="${CMAKE_INSTALL_FULL_BINDIR}/ah-flow-vm")
add_library(ah::ah ALIAS ah)
ah_compile_target(ah)
target_include_directories(ah PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_include_directories(ah PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ah PRIVATE ah_json ah_toml CURL::libcurl SQLite::SQLite3 Threads::Threads)
if(APPLE)
  target_link_libraries(ah PRIVATE "-framework CoreFoundation")
elseif(UNIX)
  target_link_libraries(ah PRIVATE m)
endif()
