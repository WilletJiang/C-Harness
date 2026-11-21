# Keep the JavaScript engine outside the SDK address space.
set(quickjs_dir "${PROJECT_SOURCE_DIR}/vendor/quickjs")
add_executable(ah_flow_vm main.c
  "${quickjs_dir}/quickjs.c" "${quickjs_dir}/cutils.c" "${quickjs_dir}/libunicode.c"
  "${quickjs_dir}/libregexp.c" "${quickjs_dir}/dtoa.c")
set_target_properties(ah_flow_vm PROPERTIES OUTPUT_NAME ah-flow-vm C_EXTENSIONS ON)
target_include_directories(ah_flow_vm PRIVATE "${quickjs_dir}")
target_compile_definitions(ah_flow_vm PRIVATE _GNU_SOURCE CONFIG_VERSION="2026-06-04")
# QuickJS's large interpreter frame under -O0 + sanitizer instrumentation
# consumes its script stack limit during built-in calls. Keep the VM optimized.
target_compile_options(ah_flow_vm PRIVATE -O2)
target_link_libraries(ah_flow_vm PRIVATE m Threads::Threads)
install(TARGETS ah_flow_vm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
