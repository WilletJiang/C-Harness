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

set(math_dir "${CMAKE_CURRENT_BINARY_DIR}/math")
set(math_lib "${math_dir}/release/${CMAKE_STATIC_LIBRARY_PREFIX}ah_tui_math${CMAKE_STATIC_LIBRARY_SUFFIX}")
add_custom_command(OUTPUT "${math_lib}"
  COMMAND "${AH_CARGO}" build --locked --release
          --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
          --target-dir "${math_dir}"
  DEPENDS Cargo.toml Cargo.lock tui_math.rs tui_math_layout.rs tui_math_png.rs
  VERBATIM)
add_custom_target(ah_tui_math_build DEPENDS "${math_lib}")
add_dependencies(ah_cli ah_tui_math_build)
target_compile_definitions(ah_cli PRIVATE AH_BUILD_TUI=1)
target_sources(ah_cli PRIVATE tui.c tui_auth.c tui_oauth.c tui_commands.c tui_events.c tui_terminal.c
                              tui_models.c tui_stream.c
                              tui_view.c tui_graphics.c tui_markdown.c tui_markdown_math.c
                              tui_markdown_table.c tui_markup_columns.c tui_markup_output.c)
target_include_directories(ah_cli PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
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
  set_tests_properties(tui_graphics PROPERTIES TIMEOUT 60)
endif()
