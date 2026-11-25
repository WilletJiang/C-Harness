include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(ah_md4c_source
  URL https://codeload.github.com/mity/md4c/tar.gz/refs/tags/release-0.5.2
  URL_HASH SHA256=55d0111d48fb11883aaee91465e642b8b640775a4d6993c2d0e7a8092758ef21
  SOURCE_SUBDIR ah-fetch-only)
FetchContent_MakeAvailable(ah_md4c_source)
add_library(ah_tui_md4c STATIC
  "${ah_md4c_source_SOURCE_DIR}/src/md4c.c"
  "${ah_md4c_source_SOURCE_DIR}/src/entity.c")

find_library(AH_TERMINFO_LIBRARY NAMES tinfo ncursesw ncurses curses REQUIRED)
find_path(AH_TERMINFO_INCLUDE_DIR term.h PATH_SUFFIXES ncursesw ncurses REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(AH_UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
find_program(AH_CARGO cargo REQUIRED)

set(math_source "${CMAKE_CURRENT_SOURCE_DIR}/render/math")
set(math_dir "${CMAKE_CURRENT_BINARY_DIR}/math")
set(math_lib "${math_dir}/release/${CMAKE_STATIC_LIBRARY_PREFIX}ah_tui_math${CMAKE_STATIC_LIBRARY_SUFFIX}")
add_custom_command(OUTPUT "${math_lib}"
  COMMAND "${AH_CARGO}" build --locked --release
          --manifest-path "${math_source}/Cargo.toml"
          --target-dir "${math_dir}"
  DEPENDS "${math_source}/Cargo.toml" "${math_source}/Cargo.lock"
          "${math_source}/src/lib.rs" "${math_source}/src/layout.rs" "${math_source}/src/png.rs"
  VERBATIM)
add_custom_target(ah_tui_math_build DEPENDS "${math_lib}")
add_dependencies(ah_cli ah_tui_math_build)
target_compile_definitions(ah_cli PRIVATE AH_BUILD_TUI=1)
target_sources(ah_cli PRIVATE
    app/app.c app/ask.c app/attachments.c app/commands.c app/events.c app/sessions.c app/stream.c
    provider/config.c provider/models.c provider/oauth.c
    terminal/terminal.c terminal/graphics.c
    view/view.c view/auth.c view/chat.c view/picker.c view/text.c
    render/markdown.c render/formula.c render/table.c render/columns.c render/output.c)
target_include_directories(ah_cli PRIVATE "${PROJECT_SOURCE_DIR}/packages"
    "${PROJECT_SOURCE_DIR}/packages/cli" "${AH_TERMINFO_INCLUDE_DIR}"
    "${ah_md4c_source_SOURCE_DIR}/src")
target_link_libraries(ah_cli PRIVATE "${math_lib}" ah_tui_md4c PkgConfig::AH_UTF8PROC
                                     "${AH_TERMINFO_LIBRARY}" ${CMAKE_DL_LIBS} m)
find_program(AH_TUI_PYTHON python3)
if(AH_TUI_PYTHON)
  target_compile_definitions(ah_cli PRIVATE AH_TUI_PYTHON="${AH_TUI_PYTHON}"
                                            AH_TUI_HOST_ROOT="${PROJECT_SOURCE_DIR}")
endif()

if(AH_TESTS_ENABLED)
  add_test(NAME tui_graphics COMMAND "${Python3_EXECUTABLE}" -B
      "${PROJECT_SOURCE_DIR}/tests/test_tui_graphics.py" $<TARGET_FILE:ah_cli>)
  add_test(NAME tui_outcome COMMAND "${Python3_EXECUTABLE}" -B
      "${PROJECT_SOURCE_DIR}/tests/test_tui_outcome.py" $<TARGET_FILE:ah_cli>)
  add_test(NAME tui_approval COMMAND "${Python3_EXECUTABLE}" -B
      "${PROJECT_SOURCE_DIR}/tests/test_tui_approval.py" $<TARGET_FILE:ah_cli>)
  add_test(NAME tui_ask COMMAND "${Python3_EXECUTABLE}" -B
      "${PROJECT_SOURCE_DIR}/tests/test_tui_ask.py" $<TARGET_FILE:ah_cli>)
  set_tests_properties(tui_graphics tui_outcome tui_approval tui_ask PROPERTIES TIMEOUT 60)
endif()
