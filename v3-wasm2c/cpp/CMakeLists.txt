cmake_minimum_required(VERSION 4.1)
project(weblox_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Source of truth: tools/ci/pins.env.  Hashes are pinned, so FetchContent
# verifies every download instead of trusting the URL or GitHub's digest.
set(PINS "${CMAKE_CURRENT_LIST_DIR}/../../tools/ci/pins.env")
file(STRINGS "${PINS}" PIN_LINES)
foreach(line IN LISTS PIN_LINES)
  if(line MATCHES "^([A-Z0-9_]+)=(.*)$")
    set(PIN_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}")
  endif()
endforeach()

if(NOT PIN_WASM_VERSION)
  message(FATAL_ERROR "pins.env has no WASM_VERSION")
endif()

if(WIN32)
  set(BUNDLE engine-windows-x64.tar.zst)
  set(SHA_PIN ${PIN_ENGINE_WINDOWS_SHA256})
elseif(APPLE)
  set(BUNDLE engine-macos-arm64.tar.zst)
  set(SHA_PIN ${PIN_ENGINE_MACOS_SHA256})
else()
  set(BUNDLE engine-linux-x64.tar.zst)
  set(SHA_PIN ${PIN_ENGINE_LINUX_SHA256})
endif()

if(NOT SHA_PIN)
  message(FATAL_ERROR "pins.env has no sha256 for ${BUNDLE}; copy it from the CI build log")
endif()

include(FetchContent)
set(RELEASE_BASE
  "https://github.com/kingdudely/Weblox/releases/download/${PIN_WASM_VERSION}")

FetchContent_Declare(weblox_engine
  URL      "${RELEASE_BASE}/${BUNDLE}"
  URL_HASH SHA256=${SHA_PIN}
)
FetchContent_MakeAvailable(weblox_engine)

# The host only needs WC.h; WC-impl.h is for recompiling generated C.
set(ENGINE_SRC "${weblox_engine_SOURCE_DIR}")
if(NOT EXISTS "${ENGINE_SRC}/WC.h")
  message(FATAL_ERROR "engine bundle is missing WC.h")
endif()

# Generate C-linkage stubs for imports we do not implement in imports.cpp.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
execute_process(
  COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/../../tools/ci/gen_stubs.py"
    "${ENGINE_SRC}/WC.h"
    "${CMAKE_CURRENT_LIST_DIR}/../tools/implemented.txt"
  OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/stubs.c"
  RESULT_VARIABLE GEN_RESULT
)
if(NOT GEN_RESULT EQUAL 0)
  message(FATAL_ERROR "gen_stubs.py failed")
endif()

add_library(wc_stubs STATIC "${CMAKE_CURRENT_BINARY_DIR}/stubs.c")
target_include_directories(wc_stubs PRIVATE "${ENGINE_SRC}")

# Imported shared library — one ABI, per-OS file names.
add_library(weblox_engine_lib SHARED IMPORTED)
if(WIN32)
  set_target_properties(weblox_engine_lib PROPERTIES
    IMPORTED_LOCATION "${ENGINE_SRC}/engine-windows-x64.dll"
    IMPORTED_IMPLIB   "${ENGINE_SRC}/engine-windows-x64.lib")
elseif(APPLE)
  set_target_properties(weblox_engine_lib PROPERTIES
    IMPORTED_LOCATION "${ENGINE_SRC}/engine-macos-arm64.dylib")
else()
  set_target_properties(weblox_engine_lib PROPERTIES
    IMPORTED_LOCATION "${ENGINE_SRC}/engine-linux-x64.so")
endif()

find_package(Threads REQUIRED)
add_executable(weblox
  host.cpp window.cpp platform.cpp net_stub.cpp net_curl.cpp
  gpu_stub.cpp fiber_stub.cpp fiber_ucontext.cpp fiber_win.cpp imports.cpp)
target_include_directories(weblox PRIVATE "${ENGINE_SRC}" "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(weblox PRIVATE wc_stubs weblox_engine_lib Threads::Threads)
