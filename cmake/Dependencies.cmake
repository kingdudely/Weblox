# All native dependencies are pinned in deps/pins.env.  System packages are
# used when available; otherwise the pinned source is built by FetchContent.
# Nothing here depends on apt/brew/choco.

include(FetchContent)
find_program(WEBLOX_CURL curl REQUIRED)

# CMake's internal downloader is fragile on codeload.github.com; download and
# verify with our own helper, then give FetchContent a local SOURCE_DIR.
# `fatal` distinguishes required dependencies (curl, SDL3) from the optional
# backends below: a failed optional download must never break the smoke test.
function(weblox_fetch_source_impl name url sha out_var fatal)
  file(MAKE_DIRECTORY "${WEBLOX_ASSET_DIR}")
  set(tar "${WEBLOX_ASSET_DIR}/${name}.tar.gz")
  set(stamp "${WEBLOX_ASSET_DIR}/${name}.${sha}.stamp")
  if(NOT EXISTS "${stamp}")
    message(STATUS "fetch ${name}: ${url} -> ${tar}")
    execute_process(COMMAND "${WEBLOX_CURL}" -fsSL --retry 5 --retry-all-errors
      -o "${tar}" "${url}"
      RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      file(REMOVE "${tar}")
      if(fatal)
        message(FATAL_ERROR "Download of ${name} failed (rc=${rc})")
      endif()
      message(STATUS "Download of ${name} failed (rc=${rc}); staying on no-op path")
      set(${out_var} "" PARENT_SCOPE)
      return()
    endif()
    file(SHA256 "${tar}" have_sha)
    if(NOT have_sha STREQUAL sha)
      file(REMOVE "${tar}")
      if(fatal)
        message(FATAL_ERROR "Hash mismatch for ${name}: ${have_sha}")
      endif()
      message(STATUS "Hash mismatch for ${name}: ${have_sha}; staying on no-op path")
      set(${out_var} "" PARENT_SCOPE)
      return()
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
      if(fatal)
        message(FATAL_ERROR "Extract of ${name} failed (rc=${rc})")
      endif()
      message(STATUS "Extract of ${name} failed (rc=${rc}); staying on no-op path")
      set(${out_var} "" PARENT_SCOPE)
      return()
    endif()
  endif()
  file(GLOB roots "${dest}/*")
  if(NOT roots)
    if(fatal)
      message(FATAL_ERROR "No source directory for ${name} in ${dest}")
    endif()
    message(STATUS "No source directory for ${name} in ${dest}; staying on no-op path")
    set(${out_var} "" PARENT_SCOPE)
    return()
  endif()
  list(GET roots 0 root)
  set(${out_var} "${root}" PARENT_SCOPE)
endfunction()

function(weblox_source_dir name url sha out_var)
  weblox_fetch_source_impl("${name}" "${url}" "${sha}" _root TRUE)
  set(${out_var} "${_root}" PARENT_SCOPE)
endfunction()

# Same pattern as weblox_source_dir, but failures are reported and the caller
# falls back to the inert logging path instead of aborting the build.
function(weblox_optional_source_dir name url sha out_var)
  weblox_fetch_source_impl("${name}" "${url}" "${sha}" _root FALSE)
  set(${out_var} "${_root}" PARENT_SCOPE)
endfunction()

# Configure and build a pinned dependency out-of-tree.  Keeping the nested
# configure in its own binary directory means a missing toolchain or a failing
# subproject cannot poison the host build; we simply do not define the
# WEBLOX_HAVE_* macro when this returns an empty archive.
function(weblox_build_static_lib name source extra_args archive_names out_var)
  set(_result "")
  if(source)
    set(_build "${WEBLOX_ASSET_DIR}/${name}-build")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${_build}"
        -G "${CMAKE_GENERATOR}" -DCMAKE_BUILD_TYPE=Release
        -DBUILD_TESTING=OFF ${extra_args}
      RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
      message(STATUS "${name} configure failed (rc=${_rc}); staying on no-op path")
      message(STATUS "${_out}\n${_err}")
    else()
      execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config Release --parallel 4
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
      if(NOT _rc EQUAL 0)
        message(STATUS "${name} build failed (rc=${_rc}); staying on no-op path")
        message(STATUS "${_out}\n${_err}")
      else()
        foreach(_archive IN LISTS archive_names)
          foreach(_dir "${_build}" "${_build}/Release" "${_build}/lib"
                       "${_build}/lib/Release")
            if(EXISTS "${_dir}/${_archive}")
              set(_result "${_dir}/${_archive}")
              break()
            endif()
          endforeach()
          if(NOT _result)
            # ngtcp2's crypto archive lands in crypto/ossl/, for example.
            file(GLOB_RECURSE _nested "${_build}/*/${_archive}")
            if(_nested)
              list(GET _nested 0 _result)
            endif()
          endif()
          if(_result)
            break()
          endif()
        endforeach()
        if(NOT _result)
          message(STATUS "${name} built but no static archive found; staying on no-op path")
        endif()
      endif()
    endif()
  endif()
  set(${out_var} "${_result}" PARENT_SCOPE)
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
  set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
  set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
  set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
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
  # CI runners lack X11/Wayland dev headers; the smoke test does not window.
  set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
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

# ---------------------------------------------------------------------------
# Dawn (WebGPU C API) - OPTIONAL.
#
# The pinned Dawn tarball is hash-verified and may be downloaded for its
# include/ tree (WEBLOX_DAWN_SOURCE=ON).  Dawn's GN build needs depot_tools and
# third_party checkouts that the tarball does not contain, so building it here
# is not self-contained.  A real backend is enabled only when a linkable
# webgpu_dawn library is found (or supplied with -DWEBLOX_DAWN_LIBRARY=...);
# without one, webgpu.c keeps the no-op logging path.
# ---------------------------------------------------------------------------
set(WEBLOX_DAWN_SOURCE OFF CACHE BOOL "fetch the pinned Dawn source tree for its headers")
set(WEBLOX_DAWN_INCLUDE_DIR "" CACHE PATH "directory containing webgpu/webgpu.h")
set(WEBLOX_DAWN_LIBRARY "" CACHE FILEPATH "linkable Dawn webgpu_dawn library")

if(WEBLOX_DAWN_SOURCE AND NOT WEBLOX_DAWN_INCLUDE_DIR)
  weblox_optional_source_dir(dawn
    "https://github.com/google/Dawn/archive/${PIN_DAWN_VERSION}.tar.gz"
    "${PIN_DAWN_SHA256}" dawn_source)
  if(dawn_source AND EXISTS "${dawn_source}/include")
    set(WEBLOX_DAWN_INCLUDE_DIR "${dawn_source}/include")
  endif()
endif()

find_path(WEBLOX_WEBGPU_HEADER webgpu/webgpu.h HINTS ${WEBLOX_DAWN_INCLUDE_DIR})
find_library(WEBLOX_WEBGPU_DAWN_LIB NAMES webgpu_dawn wgpu_native HINTS ${WEBLOX_DAWN_LIBRARY})
if(WEBLOX_DAWN_LIBRARY AND NOT WEBLOX_WEBGPU_DAWN_LIB)
  # An explicit path is authoritative; CMake's find_library may not recognise
  # a versioned or user-built archive name.
  if(EXISTS "${WEBLOX_DAWN_LIBRARY}")
    set(WEBLOX_WEBGPU_DAWN_LIB "${WEBLOX_DAWN_LIBRARY}")
  endif()
endif()
if(WEBLOX_WEBGPU_HEADER AND WEBLOX_WEBGPU_DAWN_LIB)
  set(WEBLOX_HAVE_DAWN TRUE)
  message(STATUS "Dawn backend: ${WEBLOX_WEBGPU_DAWN_LIB}")
else()
  set(WEBLOX_HAVE_DAWN FALSE)
  message(STATUS "Dawn backend unavailable; WebGPU imports remain no-op stubs")
endif()

# Surface descriptors are platform-specific; only compile them when the
# installed webgpu.h actually declares the native descriptor for this OS.
option(WEBLOX_DAWN_SURFACE "compile Dawn native-window surface support when the header provides it" ON)
set(WEBLOX_HAVE_DAWN_SURFACE FALSE)
if(WEBLOX_HAVE_DAWN AND WEBLOX_DAWN_SURFACE)
  include(CheckCSourceCompiles)
  set(CMAKE_REQUIRED_INCLUDES "${WEBLOX_WEBGPU_HEADER}")
  check_c_source_compiles("
    #include <webgpu/webgpu.h>
    int main(void) {
    #if defined(_WIN32)
      WGPUSurfaceDescriptorFromWindowsHWND s;
      s.chain.sType = WGPUSType_SurfaceDescriptorFromWindowsHWND;
    #elif defined(__APPLE__)
      WGPUSurfaceDescriptorFromMetalLayer s;
      s.chain.sType = WGPUSType_SurfaceDescriptorFromMetalLayer;
    #else
      WGPUSurfaceDescriptorFromXlibWindow s;
      s.chain.sType = WGPUSType_SurfaceDescriptorFromXlibWindow;
    #endif
      (void)s; return 0;
    }" WEBLOX_HAVE_DAWN_SURFACE)
  unset(CMAKE_REQUIRED_INCLUDES)
endif()

# ---------------------------------------------------------------------------
# nghttp3 (HTTP/3) - OPTIONAL.  Built once as a static library from the pinned
# tarball; no system package needed.
# ---------------------------------------------------------------------------
weblox_optional_source_dir(nghttp3
  "https://github.com/ngtcp2/nghttp3/archive/refs/tags/v${PIN_NGHTTP3_VERSION}.tar.gz"
  "${PIN_NGHTTP3_SHA256}" nghttp3_source)
# codeload tarballs omit git submodules.  Restore the pinned sfparse gitlink
# that nghttp3's CMake build needs, from its own hash-verified tarball.
weblox_optional_source_dir(sfparse
  "https://github.com/ngtcp2/sfparse/archive/${PIN_SFPARSE_VERSION}.tar.gz"
  "${PIN_SFPARSE_SHA256}" sfparse_source)
if(sfparse_source AND nghttp3_source AND
   NOT EXISTS "${nghttp3_source}/lib/sfparse/sfparse.c")
  file(GLOB _sfparse_children "${sfparse_source}/*")
  file(COPY ${_sfparse_children} DESTINATION "${nghttp3_source}/lib/sfparse")
endif()
weblox_build_static_lib(nghttp3 "${nghttp3_source}"
  "-DENABLE_LIB_ONLY=ON;-DENABLE_STATIC_LIB=ON;-DENABLE_SHARED_LIB=OFF;-DENABLE_WERROR=OFF"
  "libnghttp3.a;nghttp3.lib"
  WEBLOX_NGHTTP3_ARCHIVE)
if(WEBLOX_NGHTTP3_ARCHIVE)
  set(WEBLOX_HAVE_NGHTTP3 TRUE)
  set(WEBLOX_NGHTTP3_INCLUDE_DIRS
    "${nghttp3_source}/lib/includes" "${WEBLOX_ASSET_DIR}/nghttp3-build/lib/includes")
  message(STATUS "nghttp3 backend: ${WEBLOX_NGHTTP3_ARCHIVE}")
else()
  set(WEBLOX_HAVE_NGHTTP3 FALSE)
  message(STATUS "nghttp3 unavailable; WebTransport h3 layer remains no-op")
endif()

# ---------------------------------------------------------------------------
# ngtcp2 (QUIC) - OPTIONAL.  OpenSSL is used as the TLS provider when present;
# without a provider the core library still builds but the handshake path is
# disabled (WEBLOX_HAVE_NGTCP2_TLS stays undefined).
# ---------------------------------------------------------------------------
weblox_optional_source_dir(ngtcp2
  "https://github.com/ngtcp2/ngtcp2/archive/refs/tags/v${PIN_NGTCP2_VERSION}.tar.gz"
  "${PIN_NGTCP2_SHA256}" ngtcp2_source)
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND AND NOT WIN32)
  set(_ngtcp2_tls_args
    "-DENABLE_OPENSSL=ON;-DENABLE_GNUTLS=OFF;-DENABLE_BORINGSSL=OFF;-DENABLE_PICOTLS=OFF;-DENABLE_WOLFSSL=OFF")
else()
  # Schannel is not an ngtcp2 TLS backend; Windows falls back to the no-op path
  # unless the user supplies an OpenSSL build through CMAKE_PREFIX_PATH.
  set(_ngtcp2_tls_args
    "-DENABLE_OPENSSL=OFF;-DENABLE_GNUTLS=OFF;-DENABLE_BORINGSSL=OFF;-DENABLE_PICOTLS=OFF;-DENABLE_WOLFSSL=OFF")
endif()
weblox_build_static_lib(ngtcp2 "${ngtcp2_source}"
  "${_ngtcp2_tls_args};-DENABLE_LIB_ONLY=ON;-DENABLE_STATIC_LIB=ON;-DENABLE_SHARED_LIB=OFF;-DENABLE_WERROR=OFF"
  "libngtcp2.a;ngtcp2.lib"
  WEBLOX_NGTCP2_ARCHIVE)
# Same build tree: the crypto backend is produced by the configure above.
weblox_build_static_lib(ngtcp2 "${ngtcp2_source}"
  "${_ngtcp2_tls_args};-DENABLE_LIB_ONLY=ON;-DENABLE_STATIC_LIB=ON;-DENABLE_SHARED_LIB=OFF;-DENABLE_WERROR=OFF"
  "libngtcp2_crypto_ossl.a;ngtcp2_crypto_ossl.lib"
  WEBLOX_NGTCP2_CRYPTO_ARCHIVE)
if(WEBLOX_NGTCP2_ARCHIVE)
  set(WEBLOX_HAVE_NGTCP2 TRUE)
  set(WEBLOX_NGTCP2_INCLUDE_DIRS
    "${ngtcp2_source}/lib/includes"
    "${ngtcp2_source}/crypto/includes"
    "${WEBLOX_ASSET_DIR}/ngtcp2-build/lib/includes"
    "${WEBLOX_ASSET_DIR}/ngtcp2-build/crypto/includes")
  message(STATUS "ngtcp2 backend: ${WEBLOX_NGTCP2_ARCHIVE}")
else()
  set(WEBLOX_HAVE_NGTCP2 FALSE)
  message(STATUS "ngtcp2 unavailable; WebTransport QUIC layer remains no-op")
endif()
if(WEBLOX_HAVE_NGTCP2 AND WEBLOX_NGTCP2_CRYPTO_ARCHIVE AND OPENSSL_FOUND AND NOT WIN32)
  set(WEBLOX_HAVE_NGTCP2_TLS TRUE)
  message(STATUS "ngtcp2 TLS backend: ${WEBLOX_NGTCP2_CRYPTO_ARCHIVE}")
else()
  set(WEBLOX_HAVE_NGTCP2_TLS FALSE)
  message(STATUS "ngtcp2 TLS backend unavailable; connect() reports an error")
endif()
