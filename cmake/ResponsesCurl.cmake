set(_ah_bundle_curl_default OFF)
if(APPLE AND AH_TOP_LEVEL)
  set(_ah_bundle_curl_default ON)
endif()
option(AH_BUNDLE_CURL "Build the pinned WebSocket-enabled libcurl" ${_ah_bundle_curl_default})
option(AH_REQUIRE_WEBSOCKETS "Require a libcurl with WebSocket APIs" OFF)

function(ah_add_bundled_curl)
  include(FetchContent)
  if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
  endif()

  set(BUILD_TESTING OFF)
  set(BUILD_SHARED_LIBS OFF)
  set(BUILD_STATIC_LIBS ON)
  set(BUILD_CURL_EXE OFF)
  set(BUILD_EXAMPLES OFF)
  set(BUILD_LIBCURL_DOCS OFF)
  set(BUILD_MISC_DOCS OFF)
  set(ENABLE_CURL_MANUAL OFF)
  set(CURL_ENABLE_SSL ON)
  set(CURL_USE_OPENSSL ON)
  set(CURL_DISABLE_WEBSOCKETS OFF)
  set(CURL_DISABLE_INSTALL OFF)
  set(CURL_ENABLE_EXPORT_TARGET ON)
  set(CURL_DISABLE_LDAP ON)
  set(CURL_DISABLE_LDAPS ON)
  set(CURL_USE_LIBPSL OFF)
  set(CURL_USE_LIBSSH2 OFF)
  if(APPLE)
    set(USE_APPLE_SECTRUST ON)
    set(CURL_CA_BUNDLE none)
    set(CURL_CA_PATH none)
  endif()

  FetchContent_Declare(ah_curl_source
    URL https://curl.se/download/curl-8.23.0.tar.bz2
        https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.bz2
    URL_HASH SHA256=95b22eb32bf6d1fa6e3e9463bdb61cece3efc56d86576a9b3cbd428246a6d854
    TLS_VERIFY TRUE)
  FetchContent_MakeAvailable(ah_curl_source)
  install(FILES "${ah_curl_source_SOURCE_DIR}/COPYING"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ah" RENAME libcurl.txt)
endfunction()

if(AH_BUNDLE_CURL)
  ah_add_bundled_curl()
  set(AH_HAVE_CURL_WS ON)
else()
  if(AH_REQUIRE_WEBSOCKETS)
    find_package(CURL 8.16 REQUIRED COMPONENTS WSS)
  else()
    find_package(CURL REQUIRED)
  endif()
  include(CheckCSourceCompiles)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LIBRARIES CURL::libcurl)
  unset(AH_HAVE_CURL_WS CACHE)
  check_c_source_compiles("
    #include <curl/curl.h>
    #if LIBCURL_VERSION_NUM < 0x081000
    #error libcurl 8.16.0 or newer is required for WebSocket framing
    #endif
    int main(void) {
      CURL *easy = curl_easy_init();
      const struct curl_ws_frame *frame = 0;
      char byte;
      size_t n;
      return curl_ws_send(easy, &byte, 1, &n, 0, CURLWS_TEXT) |
             curl_ws_recv(easy, &byte, 1, &n, &frame);
    }" AH_HAVE_CURL_WS)
  cmake_pop_check_state()
  if(AH_REQUIRE_WEBSOCKETS AND NOT AH_HAVE_CURL_WS)
    message(FATAL_ERROR "Select a WebSocket-enabled libcurl 8.16 or newer with CURL_DIR or CMAKE_PREFIX_PATH")
  endif()
endif()
