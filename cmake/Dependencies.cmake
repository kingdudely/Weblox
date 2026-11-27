# All native dependencies are pinned in deps/pins.env.  System packages are
# used when available; otherwise the pinned source is built by FetchContent.
# Nothing here depends on apt/brew/choco.

include(FetchContent)

# CMake's internal downloader is fragile on codeload.github.com; download and
# verify with our own helper, then give FetchContent a local SOURCE_DIR.
function(weblox_source_dir name url sha out_var)
  file(MAKE_DIRECTORY "${WEBLOX_ASSET_DIR}")
  set(tar "${WEBLOX_ASSET_DIR}/${name}.tar.gz")
  set(stamp "${WEBLOX_ASSET_DIR}/${name}.${sha}.stamp")
  if(NOT EXISTS "${stamp}")
    find_program(WEBLOX_CURL curl REQUIRED)
    message(STATUS "fetch ${name}: ${url} -> ${tar}")
    execute_process(COMMAND "${WEBLOX_CURL}" -fsSL --retry 5 --retry-all-errors
      -o "${tar}" "${url}"
      RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      file(REMOVE "${tar}")
      message(FATAL_ERROR "Download of ${name} failed (rc=${rc})")
    endif()
    file(SHA256 "${tar}" have_sha)
    if(NOT have_sha STREQUAL sha)
      file(REMOVE "${tar}")
      message(FATAL_ERROR "Hash mismatch for ${name}: ${have_sha}")
    endif()
    file(WRITE "${stamp}" "ok\n")
  endif()

  set(dest "${WEBLOX_ASSET_DIR}/${name}-src")
  file(GLOB roots "${dest}/*")
  if(NOT roots)
    file(MAKE_DIRECTORY "${dest}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xzf "${tar}"
      WORKING_DIRECTORY "${dest}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "Extract of ${name} failed (rc=${rc})")
    endif()
  endif()
  file(GLOB roots "${dest}/*")
  if(NOT roots)
    message(FATAL_ERROR "No source directory for ${name} in ${dest}")
  endif()
  list(GET roots 0 root)
  set(${out_var} "${root}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
# libcurl (HTTP + URL API)
# ---------------------------------------------------------------------------
find_package(CURL QUIET)
if(CURL_FOUND)
  set(WEBLOX_CURL_LIB CURL::libcurl)
else()
  message(STATUS "libcurl not found; building pinned curl ${PIN_CURL_VERSION}")
  set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(CURL_USE_ZSTD OFF CACHE BOOL "" FORCE)
  if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
  elseif(APPLE)
    set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
  endif()
  weblox_source_dir(curl
    "https://github.com/curl/curl/archive/refs/tags/curl-${PIN_CURL_VERSION}.tar.gz"
    "${PIN_CURL_SHA256}" curl_source)
  FetchContent_Declare(curl SOURCE_DIR "${curl_source}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
  FetchContent_MakeAvailable(curl)
  if(TARGET CURL::libcurl)
    set(WEBLOX_CURL_LIB CURL::libcurl)
  else()
    set(WEBLOX_CURL_LIB libcurl)
  endif()
  set(CURL_FOUND TRUE)
endif()

# ---------------------------------------------------------------------------
# SDL3 (window, input, audio, threads, sync, timers)
# ---------------------------------------------------------------------------
find_package(SDL3 QUIET)
if(SDL3_FOUND)
  set(WEBLOX_SDL3_LIB SDL3::SDL3)
else()
  message(STATUS "SDL3 not found; building pinned SDL ${PIN_SDL3_VERSION}")
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
  weblox_source_dir(sdl3
    "https://github.com/libsdl-org/SDL/archive/refs/tags/release-${PIN_SDL3_VERSION}.tar.gz"
    "${PIN_SDL3_SHA256}" sdl3_source)
  FetchContent_Declare(sdl3 SOURCE_DIR "${sdl3_source}" DOWNLOAD_COMMAND "" UPDATE_COMMAND "")
  FetchContent_MakeAvailable(sdl3)
  if(TARGET SDL3::SDL3-static)
    set(WEBLOX_SDL3_LIB SDL3::SDL3-static)
  else()
    set(WEBLOX_SDL3_LIB SDL3::SDL3)
  endif()
  set(SDL3_FOUND TRUE)
endif()
