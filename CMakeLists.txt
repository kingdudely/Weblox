cmake_minimum_required(VERSION 3.20)
project(weblox LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Pins: the only source of truth for downloads.
# ---------------------------------------------------------------------------
set(PINS "${CMAKE_CURRENT_LIST_DIR}/deps/pins.env")
file(STRINGS "${PINS}" PIN_LINES)
foreach(line IN LISTS PIN_LINES)
  if(line MATCHES "^([A-Z0-9_]+)=(.*)$")
    set(PIN_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()
if(NOT PIN_WASM_VERSION)
  message(FATAL_ERROR "deps/pins.env has no WASM_VERSION")
endif()
set(WEBLOX_RELEASE_BASE
  "https://github.com/kingdudely/Weblox/releases/download/${PIN_WASM_VERSION}")

set(WEBLOX_ENGINE_MODE "prebuilt" CACHE STRING "prebuilt shared library or generated C source")
set_property(CACHE WEBLOX_ENGINE_MODE PROPERTY STRINGS prebuilt source)
# A source-mode build is only a validation gate; O0 keeps it fast.  Shipped
# bundles are built at O3 by build-engine.yml.
set(WEBLOX_ENGINE_OPT "O0" CACHE STRING "optimization for locally compiled generated C")
set_property(CACHE WEBLOX_ENGINE_OPT PROPERTY STRINGS O0 O1 O2 O3)

if(WIN32)
  set(BUNDLE engine-windows-x64.tar.zst)
  set(BUNDLE_SHA ${PIN_ENGINE_WINDOWS_SHA256})
  set(ENGINE_SHARED engine-windows-x64.dll)
  set(ENGINE_IMPLIB engine-windows-x64.lib)
elseif(APPLE)
  set(BUNDLE engine-macos-arm64.tar.zst)
  set(BUNDLE_SHA ${PIN_ENGINE_MACOS_SHA256})
  set(ENGINE_SHARED engine-macos-arm64.dylib)
else()
  set(BUNDLE engine-linux-x64.tar.zst)
  set(BUNDLE_SHA ${PIN_ENGINE_LINUX_SHA256})
  set(ENGINE_SHARED engine-linux-x64.so)
endif()

# ---------------------------------------------------------------------------
# Engine payload.
# ---------------------------------------------------------------------------
include(cmake/FetchAsset.cmake)
include(cmake/Dependencies.cmake)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(WABT_DIR "${CMAKE_CURRENT_LIST_DIR}/deps/wabt")

# HTTP uses the pinned libcurl from cmake/Dependencies.cmake.
set(HTTP_SOURCES "")
set(IMPLEMENTED_FILE "${CMAKE_CURRENT_LIST_DIR}/tools/implemented.txt")
if(CURL_FOUND)
  list(APPEND HTTP_SOURCES src/http.c src/fetch.c src/fs.c)
  file(READ "${IMPLEMENTED_FILE}" IMPLEMENTED_TEXT)
  string(CONCAT HTTP_IMPORTS
    "emscripten_start_fetch\n"
    "emscripten_fetch_free\n"
    "0x5Femscripten_fetch_get_response_headers_length\n"
    "0x5Femscripten_fetch_get_response_headers\n"
    "js_fetch_file\n"
    "0x5F_syscall_getegid32\n"
    "0x5F_syscall_geteuid32\n"
    "0x5F_syscall_getgid32\n"
    "0x5F_syscall_getuid32\n")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/implemented-host.txt"
    "${IMPLEMENTED_TEXT}${HTTP_IMPORTS}")
  set(IMPLEMENTED_FILE "${CMAKE_CURRENT_BINARY_DIR}/implemented-host.txt")

  set(YYJSON_DIR "${WEBLOX_ASSET_DIR}/yyjson")
  if(NOT EXISTS "${YYJSON_DIR}/src/yyjson.c")
    set(yyjson_tar "${WEBLOX_ASSET_DIR}/yyjson.tar.gz")
    file(DOWNLOAD
      "https://github.com/ibireme/yyjson/archive/refs/tags/${PIN_YYJSON_VERSION}.tar.gz"
      "${yyjson_tar}" EXPECTED_HASH SHA256=${PIN_YYJSON_SHA256}
      TLS_VERIFY ON TIMEOUT 600 STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "yyjson download failed: ${status}")
    endif()
    file(MAKE_DIRECTORY "${YYJSON_DIR}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${yyjson_tar}"
      WORKING_DIRECTORY "${YYJSON_DIR}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "yyjson extract failed (rc=${rc})")
    endif()
    file(GLOB yyjson_root "${YYJSON_DIR}/yyjson-*")
    if(yyjson_root)
      file(GLOB yyjson_children "${yyjson_root}/*")
      file(COPY ${yyjson_children} DESTINATION "${YYJSON_DIR}")
      file(REMOVE_RECURSE ${yyjson_root})
    endif()
  endif()
  add_library(yyjson STATIC "${YYJSON_DIR}/src/yyjson.c")
  target_include_directories(yyjson PUBLIC "${YYJSON_DIR}/src")
else()
  message(STATUS "libcurl not found; HTTP imports remain no-op stubs")
endif()

# Fibers, atomics and workers need SDL3 and minicoro.
set(FIBER_SOURCES "")
if(SDL3_FOUND)
  list(APPEND FIBER_SOURCES
    src/fibers.c src/atomics.c src/thread.c src/futex.c
    src/mailbox.c src/deferred.c)
  file(READ "${IMPLEMENTED_FILE}" IMPLEMENTED_TEXT2)
  string(CONCAT FIBER_IMPORTS
    "0x5F_asyncjs_0x5Fcreate_suspended_fiber_js\n"
    "0x5F_asyncjs_0x5Ffiber_transfer_js\n"
    "fiber_destroy_js\n"
    "0x5F_asyncjs_0x5FrbxFutexWaitMainJspi\n"
    "0x5F_pthread_create_js\n"
    "0x5Femscripten_init_main_thread_js\n"
    "0x5Femscripten_thread_cleanup\n"
    "0x5Femscripten_thread_mailbox_await\n"
    "0x5Femscripten_notify_mailbox_postmessage\n"
    "emscripten_async_call\n"
    "emscripten_unwind_to_js_event_loop\n"
    "schedule_main_loop\n")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/implemented-host2.txt"
    "${IMPLEMENTED_TEXT2}${FIBER_IMPORTS}")
  set(IMPLEMENTED_FILE "${CMAKE_CURRENT_BINARY_DIR}/implemented-host2.txt")
else()
  message(STATUS "SDL3 not found; fiber imports remain no-op stubs")
endif()

set(ENGINE_SRC "${CMAKE_BINARY_DIR}/engine")
file(MAKE_DIRECTORY "${ENGINE_SRC}")

if(WEBLOX_ENGINE_MODE STREQUAL "prebuilt")
  weblox_fetch_asset("${BUNDLE}" "${BUNDLE_SHA}" "${ENGINE_SRC}")
  weblox_fetch_asset(WC.h "${PIN_WC_H_SHA256}" "${ENGINE_SRC}")
  if(NOT EXISTS "${ENGINE_SRC}/${ENGINE_SHARED}")
    message(FATAL_ERROR "engine bundle is missing ${ENGINE_SHARED}")
  endif()
elseif(WEBLOX_ENGINE_MODE STREQUAL "source")
  weblox_fetch_asset(generated-c.tar.zst "${PIN_GENERATED_C_SHA256}" "${ENGINE_SRC}")
  file(GLOB GENERATED_C "${ENGINE_SRC}/WC_*.c")
  if(NOT GENERATED_C)
    message(FATAL_ERROR "generated-c.tar.zst contains no WC_*.c files")
  endif()
  # SIMDE supplies the wasm SIMD intrinsics used by the generated C.
  set(SIMDE_DIR "${WEBLOX_ASSET_DIR}/simde")
  if(NOT EXISTS "${SIMDE_DIR}/simde/wasm/simd128.h")
    set(simde_tar "${WEBLOX_ASSET_DIR}/simde.tar.gz")
    file(DOWNLOAD
      "https://github.com/simd-everywhere/simde/archive/refs/tags/v0.8.2.tar.gz"
      "${simde_tar}" EXPECTED_HASH SHA256=${PIN_SIMDE_SHA256}
      TLS_VERIFY ON TIMEOUT 600 STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "SIMDE download failed: ${status}")
    endif()
    file(MAKE_DIRECTORY "${SIMDE_DIR}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${simde_tar}"
      WORKING_DIRECTORY "${SIMDE_DIR}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "SIMDE extract failed (rc=${rc})")
    endif()
    file(GLOB simde_root "${SIMDE_DIR}/simde-*")
    if(simde_root)
      file(GLOB simde_children "${simde_root}/*")
      file(COPY ${simde_children} DESTINATION "${SIMDE_DIR}")
      file(REMOVE_RECURSE ${simde_root})
    endif()
  endif()
else()
  message(FATAL_ERROR "WEBLOX_ENGINE_MODE must be prebuilt or source")
endif()

# The published WC-impl.h has no-op atomic wait/notify stubs.  In source mode
# we compile the generated C ourselves, so a build-directory VFS overlay can
# fix it without touching the Release asset.  A prebuilt .so/.dylib/.dll has
# the stubs baked in and must be rebuilt with this overlay by build-engine.yml.
if(WEBLOX_ENGINE_MODE STREQUAL "source")
  set(WC_IMPL "${ENGINE_SRC}/WC-impl.h")
  if(NOT EXISTS "${WC_IMPL}")
    message(FATAL_ERROR "generated-c bundle is missing WC-impl.h")
  endif()
  execute_process(
    COMMAND ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_LIST_DIR}/tools/make_overlay.py"
      "${WC_IMPL}" "${CMAKE_CURRENT_BINARY_DIR}/overlay"
    OUTPUT_VARIABLE ATOMIC_OVERLAY
    RESULT_VARIABLE rc
    ERROR_VARIABLE overlay_error)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "make_overlay.py failed: ${overlay_error}")
  endif()
  string(STRIP "${ATOMIC_OVERLAY}" ATOMIC_OVERLAY)
  message(STATUS "atomic overlay: ${ATOMIC_OVERLAY}")
endif()

if(NOT EXISTS "${ENGINE_SRC}/WC.h")
  message(FATAL_ERROR "engine is missing WC.h")
endif()

# ---------------------------------------------------------------------------
# Import stubs generated from WC.h.  No import indices anywhere.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(STUBS "${CMAKE_CURRENT_BINARY_DIR}/stubs.c")
execute_process(
  COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/tools/gen_stubs.py"
    "${ENGINE_SRC}/WC.h"
    "${IMPLEMENTED_FILE}"
  OUTPUT_FILE "${STUBS}"
  RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "gen_stubs.py failed")
endif()
# OBJECT so the engine library's unresolved imports are always satisfied.
add_library(wc_stubs OBJECT "${STUBS}")
target_include_directories(wc_stubs PUBLIC "${ENGINE_SRC}" "${WABT_DIR}")

# ---------------------------------------------------------------------------
# WABT runtime: only needed where the engine library does not export it.
# ---------------------------------------------------------------------------
add_library(wc_runtime STATIC
  "${WABT_DIR}/wasm-rt-impl.c"
  "${WABT_DIR}/wasm-rt-mem-impl.c"
  "${WABT_DIR}/wasm-rt-exceptions-impl.c")
target_include_directories(wc_runtime PUBLIC "${WABT_DIR}")

if(WEBLOX_ENGINE_MODE STREQUAL "source")
  add_library(wc_generated STATIC ${GENERATED_C})
  target_include_directories(wc_generated PRIVATE
    "${ENGINE_SRC}" "${WABT_DIR}" "${SIMDE_DIR}")
  target_compile_options(wc_generated PRIVATE
    -w "-${WEBLOX_ENGINE_OPT}" -ivfsoverlay "${ATOMIC_OVERLAY}")
endif()

# ---------------------------------------------------------------------------
# Host binary.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

add_executable(weblox
  src/main.c src/mem.c src/imports.c ${HTTP_SOURCES} ${FIBER_SOURCES}
  src/window_sdl.c src/gl.c src/webgpu.c src/webtransport.c)
target_include_directories(weblox PRIVATE
  "${ENGINE_SRC}" "${WABT_DIR}" "${CMAKE_CURRENT_LIST_DIR}/src"
  "${CMAKE_CURRENT_LIST_DIR}/deps/minicoro")
target_link_libraries(weblox PRIVATE wc_stubs Threads::Threads m)
if(SDL3_FOUND)
  target_compile_definitions(weblox PRIVATE WEBLOX_HAVE_SDL3=1)
  target_link_libraries(weblox PRIVATE ${WEBLOX_SDL3_LIB})
endif()
if(CURL_FOUND)
  target_link_libraries(weblox PRIVATE ${WEBLOX_CURL_LIB} yyjson)
endif()

if(WEBLOX_ENGINE_MODE STREQUAL "source")
  target_compile_definitions(weblox PRIVATE WEBLOX_SOURCE_MODE=1)
  target_link_libraries(weblox PRIVATE wc_generated)
elseif(WIN32)
  target_link_options(weblox PRIVATE "-Wl,/STACK:8388608")
  add_library(weblox_engine_lib SHARED IMPORTED)
  set_target_properties(weblox_engine_lib PROPERTIES
    IMPORTED_LOCATION "${ENGINE_SRC}/${ENGINE_SHARED}"
    IMPORTED_IMPLIB "${ENGINE_SRC}/${ENGINE_IMPLIB}")
  target_link_libraries(weblox PRIVATE weblox_engine_lib wc_runtime)
  add_custom_command(TARGET weblox POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${ENGINE_SRC}/${ENGINE_SHARED}" "$<TARGET_FILE_DIR:weblox>/${ENGINE_SHARED}")
elseif(APPLE)
  # Apple's linker does not accept -l:filename.  The published dylib also has
  # an absolute install name, so rewrite a local copy to @rpath.
  set(ENGINE_RUNTIME "${CMAKE_CURRENT_BINARY_DIR}/${ENGINE_SHARED}")
  configure_file("${ENGINE_SRC}/${ENGINE_SHARED}" "${ENGINE_RUNTIME}" COPYONLY)
  find_program(INSTALL_NAME_TOOL install_name_tool REQUIRED)
  execute_process(
    COMMAND "${INSTALL_NAME_TOOL}" -id "@rpath/${ENGINE_SHARED}" "${ENGINE_RUNTIME}"
    RESULT_VARIABLE rc)
  if(NOT rc EQUAL 0)
    message(FATAL_ERROR "install_name_tool failed")
  endif()
  target_link_libraries(weblox PRIVATE "${ENGINE_RUNTIME}")
  set_target_properties(weblox PROPERTIES BUILD_RPATH "@loader_path")
else()
  # No SONAME in the published .so: link by bare name so DT_NEEDED is a plain
  # filename resolved through the rpath, not a cwd-relative path.
  target_link_directories(weblox PRIVATE "${ENGINE_SRC}")
  target_link_libraries(weblox PRIVATE "-l:${ENGINE_SHARED}")
  set_target_properties(weblox PROPERTIES BUILD_RPATH "$ORIGIN/engine")
endif()

enable_testing()
add_test(NAME smoke COMMAND weblox)
