# Only host executables install process-wide signal handlers.
add_library(ah_app_signals OBJECT signals.c)
ah_compile_target(ah_app_signals)
target_include_directories(ah_app_signals PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ah_app_signals PRIVATE ah Threads::Threads)

if(AH_BUILD_CLI)
  set(ah_cli_sources
    main.c options.c output.c rpc.c credential.c
    extensions.c content.c policies.c)
  add_executable(ah_cli ${ah_cli_sources})
  set_target_properties(ah_cli PROPERTIES OUTPUT_NAME ah)
  ah_compile_target(ah_cli)
  target_include_directories(ah_cli PRIVATE "${PROJECT_SOURCE_DIR}/packages/agent/src")
  target_link_libraries(ah_cli PRIVATE ah_app_signals ah ah_json Threads::Threads)
  install(TARGETS ah_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  if(AH_TESTS_ENABLED AND AH_HAVE_CURL_WS)
    # Replace the I/O object from the static archive in this test binary only.
    add_executable(ah_ws_fault_fixture ${ah_cli_sources}
      "${PROJECT_SOURCE_DIR}/packages/agent/src/providers/responses_ws_io.c")
    ah_compile_target(ah_ws_fault_fixture)
    target_compile_definitions(ah_ws_fault_fixture PRIVATE AH_HAVE_CURL_WS=1 AH_WS_IO_TEST=1)
    target_include_directories(ah_ws_fault_fixture PRIVATE "${PROJECT_SOURCE_DIR}/packages/agent/src")
    target_link_libraries(ah_ws_fault_fixture PRIVATE ah_app_signals ah ah_json Threads::Threads CURL::libcurl)
  endif()
endif()
