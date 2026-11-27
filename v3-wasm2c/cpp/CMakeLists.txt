cmake_minimum_required(VERSION 3.20)
project(weblox_host LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# The vendored WABT runtime uses C11 atomics; C++ needs the define below.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Pins.  tools/ci/pins.env is the single source of truth for every download.
# ---------------------------------------------------------------------------
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

set(WEBLOX_ENGINE_MODE "prebuilt" CACHE STRING
  "prebuilt: pinned shared library; source: pinned generated C")
set_property(CACHE WEBLOX_ENGINE_MODE PROPERTY STRINGS prebuilt source)

if(WIN32)
  set(BUNDLE engine-windows-x64.tar.zst)
  set(SHA_PIN ${PIN_ENGINE_WINDOWS_SHA256})
  set(ENGINE_SHARED engine-windows-x64.dll)
  set(ENGINE_IMPLIB engine-windows-x64.lib)
elseif(APPLE)
  set(BUNDLE engine-macos-arm64.tar.zst)
  set(SHA_PIN ${PIN_ENGINE_MACOS_SHA256})
  set(ENGINE_SHARED engine-macos-arm64.dylib)
else()
  set(BUNDLE engine-linux-x64.tar.zst)
  set(SHA_PIN ${PIN_ENGINE_LINUX_SHA256})
  set(ENGINE_SHARED engine-linux-x64.so)
endif()
set(RELEASE_BASE
  "https://github.com/kingdudely/Weblox/releases/download/${PIN_WASM_VERSION}")

# ---------------------------------------------------------------------------
# Hash-verified download helper.  Works with stock CMake (>= 3.20): the
# .tar.zst is unpacked with the zstd CLI and CMake's own tar extractor, so no
# CMake 4.1 libarchive support and no FetchContent version pinning is needed.
# ---------------------------------------------------------------------------
find_program(ZSTD_EXECUTABLE zstd REQUIRED)
set(ASSET_DIR "${CMAKE_BINARY_DIR}/_assets")

function(weblox_fetch_asset filename sha destination)
  if(NOT sha)
    message(FATAL_ERROR "pins.env has no sha256 for ${filename}")
  endif()
  set(_archive "${ASSET_DIR}/${filename}")
  set(_stamp "${ASSET_DIR}/${filename}.${sha}.stamp")
  if(EXISTS "${_stamp}")
    return()
  endif()

  file(MAKE_DIRECTORY "${ASSET_DIR}" "${destination}")
  if(EXISTS "${_archive}")
    file(SHA256 "${_archive}" _have)
    if(NOT _have STREQUAL sha)
      file(REMOVE "${_archive}")
    endif()
  endif()
  if(NOT EXISTS "${_archive}")
    message(STATUS "Downloading ${filename} from ${RELEASE_BASE}")
    file(DOWNLOAD "${RELEASE_BASE}/${filename}" "${_archive}"
      EXPECTED_HASH SHA256=${sha}
      TLS_VERIFY ON TIMEOUT 1200 SHOW_PROGRESS STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      list(GET _status 1 _msg)
      file(REMOVE "${_archive}")
      message(FATAL_ERROR "Download of ${filename} failed: ${_msg}")
    endif()
  endif()

  if(filename MATCHES "\\.tar\\.zst$")
    get_filename_component(_tarfile "${filename}" NAME_WE)
    set(_tarfile "${ASSET_DIR}/${_tarfile}")
    execute_process(
      COMMAND "${ZSTD_EXECUTABLE}" -q -f -d "${_archive}" -o "${_tarfile}"
      RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "zstd failed to decompress ${filename} (rc=${_rc})")
    endif()
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -E tar xf "${_tarfile}"
      WORKING_DIRECTORY "${destination}"
      RESULT_VARIABLE _rc)
    file(REMOVE "${_tarfile}")
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "Failed to extract ${filename} (rc=${_rc})")
    endif()
  else()
    configure_file("${_archive}" "${destination}/${filename}" COPYONLY)
  endif()
  file(WRITE "${_stamp}" "ok\n")
endfunction()

# ---------------------------------------------------------------------------
# Engine and headers.  ABI always comes from the generated WC.h; no indices.
# ---------------------------------------------------------------------------
set(ENGINE_SRC "${CMAKE_BINARY_DIR}/engine")
file(MAKE_DIRECTORY "${ENGINE_SRC}")

if(WEBLOX_ENGINE_MODE STREQUAL "source")
  weblox_fetch_asset(generated-c.tar.zst "${PIN_GENERATED_C_SHA256}" "${ENGINE_SRC}")
  file(GLOB GENERATED_C "${ENGINE_SRC}/WC_*.c")
  if(NOT GENERATED_C)
    message(FATAL_ERROR "generated-c.tar.zst contains no WC_*.c files")
  endif()
  # SIMDE provides the wasm SIMD intrinsics the generated C uses on x86.
  set(SIMDE_DIR "${ASSET_DIR}/simde")
  if(NOT EXISTS "${SIMDE_DIR}/simde/wasm/simd128.h")
    set(_simde_tar "${ASSET_DIR}/simde.tar.gz")
    file(DOWNLOAD
      "https://github.com/simd-everywhere/simde/archive/refs/tags/v0.8.2.tar.gz"
      "${_simde_tar}" EXPECTED_HASH SHA256=${PIN_SIMDE_SHA256}
      TLS_VERIFY ON TIMEOUT 600 STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      message(FATAL_ERROR "SIMDE download failed: ${_status}")
    endif()
    file(MAKE_DIRECTORY "${SIMDE_DIR}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${_simde_tar}"
      WORKING_DIRECTORY "${SIMDE_DIR}" RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "SIMDE extraction failed (rc=${_rc})")
    endif()
    file(GLOB _simde_root "${SIMDE_DIR}/simde-*")
    if(_simde_root)
      file(GLOB _simde_children "${_simde_root}/*")
      file(COPY ${_simde_children} DESTINATION "${SIMDE_DIR}")
      file(REMOVE_RECURSE ${_simde_root})
    endif()
  endif()
elseif(WEBLOX_ENGINE_MODE STREQUAL "prebuilt")
  weblox_fetch_asset("${BUNDLE}" "${SHA_PIN}" "${ENGINE_SRC}")
  weblox_fetch_asset(WC.h "${PIN_WC_H_SHA256}" "${ENGINE_SRC}")
  if(NOT EXISTS "${ENGINE_SRC}/${ENGINE_SHARED}")
    message(FATAL_ERROR "engine bundle is missing ${ENGINE_SHARED}")
  endif()
else()
  message(FATAL_ERROR "WEBLOX_ENGINE_MODE must be prebuilt or source")
endif()

if(NOT EXISTS "${ENGINE_SRC}/WC.h")
  message(FATAL_ERROR "engine sources are missing WC.h")
endif()

# ---------------------------------------------------------------------------
# Import stubs: generated from WC.h, never from hardcoded indices.
# ---------------------------------------------------------------------------
find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(STUBS "${CMAKE_CURRENT_BINARY_DIR}/stubs.c")
execute_process(
  COMMAND ${Python3_EXECUTABLE}
    "${CMAKE_CURRENT_LIST_DIR}/../../tools/ci/gen_stubs.py"
    "${ENGINE_SRC}/WC.h"
    "${CMAKE_CURRENT_LIST_DIR}/implemented-cpp.txt"
  OUTPUT_FILE "${STUBS}"
  RESULT_VARIABLE _gen_result)
if(NOT _gen_result EQUAL 0)
  message(FATAL_ERROR "gen_stubs.py failed for ${ENGINE_SRC}/WC.h")
endif()
# OBJECT, not STATIC: the engine .so imports these symbols.  A static archive
# scanned before the .so would have no reason to pull its members in.
add_library(wc_stubs OBJECT "${STUBS}")
target_include_directories(wc_stubs PUBLIC "${ENGINE_SRC}" "${CMAKE_CURRENT_LIST_DIR}/abi")

# ---------------------------------------------------------------------------
# WABT runtime: compiled into the host so Windows does not need wasm_rt_*
# exports from the DLL.  Vendored 1.0.42 sources match the engine's ABI.
# ---------------------------------------------------------------------------
set(ABI_DIR "${CMAKE_CURRENT_LIST_DIR}/abi")
add_library(wc_runtime STATIC
  "${ABI_DIR}/wasm-rt-impl.c"
  "${ABI_DIR}/wasm-rt-mem-impl.c"
  "${ABI_DIR}/wasm-rt-exceptions-impl.c")
target_include_directories(wc_runtime PUBLIC "${ABI_DIR}")
if(MSVC)
  # clang-cl does not define __STDC_VERSION__, so wasm-rt.h would omit the C11
  # structs that the generated library expects.
  target_compile_definitions(wc_runtime PRIVATE WASM_RT_C11_AVAILABLE=1)
else()
  target_compile_options(wc_runtime PRIVATE -w -fno-asynchronous-unwind-tables)
endif()

if(WEBLOX_ENGINE_MODE STREQUAL "source")
  add_library(wc_generated STATIC ${GENERATED_C})
  target_include_directories(wc_generated PRIVATE
    "${ENGINE_SRC}" "${ABI_DIR}" "${SIMDE_DIR}")
  if(NOT MSVC)
    target_compile_options(wc_generated PRIVATE
      -w -fno-stack-protector -fno-asynchronous-unwind-tables -fPIC)
  endif()
endif()

# ---------------------------------------------------------------------------
# Host executable.  Platform implementations are selected by compile
# definitions; the default is the portable stub set.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)
add_executable(weblox
  host.cpp imports.cpp asm_const.cpp thread_init.cpp host_info.cpp loop.cpp platform.cpp mem.c
  window.cpp window_sdl.cpp net_stub.cpp net_curl.cpp
  gpu_stub.cpp fiber_stub.cpp fiber_ucontext.cpp fiber_win.cpp)
target_include_directories(weblox PRIVATE
  "${ENGINE_SRC}" "${ABI_DIR}" "${CMAKE_CURRENT_LIST_DIR}")
# C++ files that include WC.h include cpp_compat.h first (see the header).
if(WIN32)
  target_compile_definitions(weblox PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
  # clang from the runner image is older than the MSVC STL version gate.
  target_compile_definitions(weblox PRIVATE _ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
endif()
target_link_libraries(weblox PRIVATE
  wc_stubs Threads::Threads ${CMAKE_DL_LIBS})
# The Linux .so and macOS .dylib export their own WABT runtime.  Using the
# host's copy there would split exception/TLS state and crash on wasm throws.
# The Windows DLL exports only WC.h names, so the host must supply its own.
if(WIN32)
  target_link_libraries(weblox PRIVATE wc_runtime)
endif()

if(WEBLOX_ENGINE_MODE STREQUAL "source")
  target_link_libraries(weblox PRIVATE wc_generated)
elseif(WIN32)
  add_library(weblox_engine_lib SHARED IMPORTED)
  set_target_properties(weblox_engine_lib PROPERTIES
    IMPORTED_LOCATION "${ENGINE_SRC}/${ENGINE_SHARED}"
    IMPORTED_IMPLIB "${ENGINE_SRC}/${ENGINE_IMPLIB}")
  target_link_libraries(weblox PRIVATE weblox_engine_lib)
  # Windows searches for DLLs next to the executable, not in the link dir.
  add_custom_command(TARGET weblox POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${ENGINE_SRC}/${ENGINE_SHARED}" "$<TARGET_FILE_DIR:weblox>/${ENGINE_SHARED}")
elseif(APPLE)
  # The released dylib has the build runner's absolute path as its install
  # name.  Rewrite a local copy to @rpath so the host is relocatable; this
  # avoids changing the published binary.
  set(ENGINE_RUNTIME "${CMAKE_CURRENT_BINARY_DIR}/${ENGINE_SHARED}")
  configure_file("${ENGINE_SRC}/${ENGINE_SHARED}" "${ENGINE_RUNTIME}" COPYONLY)
  find_program(INSTALL_NAME_TOOL install_name_tool REQUIRED)
  execute_process(
    COMMAND "${INSTALL_NAME_TOOL}" -id "@rpath/${ENGINE_SHARED}" "${ENGINE_RUNTIME}"
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "install_name_tool failed for ${ENGINE_SHARED}")
  endif()
  target_link_libraries(weblox PRIVATE "${ENGINE_RUNTIME}")
  set_target_properties(weblox PROPERTIES BUILD_RPATH "@loader_path")
else()
  # Link by bare filename: the prebuilt .so has no SONAME, so passing a path
  # would make the loader interpret DT_NEEDED relative to the cwd.  -l:
  # keeps DT_NEEDED as a plain name, resolved through the rpath below.
  target_link_directories(weblox PRIVATE "${ENGINE_SRC}")
  target_link_libraries(weblox PRIVATE "-l:${ENGINE_SHARED}")
  set_target_properties(weblox PROPERTIES BUILD_RPATH "$ORIGIN/engine")
endif()

enable_testing()
add_test(NAME smoke COMMAND weblox)
