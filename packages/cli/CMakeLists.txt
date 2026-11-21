# Only host executables install process-wide signal handlers.
add_library(ah_app_signals OBJECT signals.c)
ah_compile_target(ah_app_signals)
target_include_directories(ah_app_signals PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ah_app_signals PRIVATE ah Threads::Threads)

if(AH_BUILD_CLI)
  add_executable(ah_cli
    main.c options.c output.c rpc.c credential.c
    extensions.c content.c policies.c)
  set_target_properties(ah_cli PROPERTIES OUTPUT_NAME ah)
  ah_compile_target(ah_cli)
  target_include_directories(ah_cli PRIVATE "${PROJECT_SOURCE_DIR}/packages/agent/src")
  target_link_libraries(ah_cli PRIVATE ah_app_signals ah ah_json Threads::Threads)
  install(TARGETS ah_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
