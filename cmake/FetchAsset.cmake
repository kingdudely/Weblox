# Hash-verified download of a pinned Release asset.
#
# Stock CMake 3.20+ only: file(DOWNLOAD ... EXPECTED_HASH) verifies the bytes,
# zstd decompresses, and CMake's own tar extractor unpacks.  No FetchContent
# version pinning, no CMake 4.1 libarchive requirement, no trust in the URL.

find_program(ZSTD_EXECUTABLE zstd REQUIRED)
set(WEBLOX_ASSET_DIR "${CMAKE_BINARY_DIR}/assets")

function(weblox_fetch_asset filename sha destination)
  if(NOT sha)
    message(FATAL_ERROR "deps/pins.env has no hash for ${filename}")
  endif()
  set(archive "${WEBLOX_ASSET_DIR}/${filename}")
  set(stamp "${WEBLOX_ASSET_DIR}/${filename}.${sha}.stamp")
  if(EXISTS "${stamp}")
    return()
  endif()

  file(MAKE_DIRECTORY "${WEBLOX_ASSET_DIR}" "${destination}")
  if(EXISTS "${archive}")
    file(SHA256 "${archive}" have)
    if(NOT have STREQUAL sha)
      file(REMOVE "${archive}")
    endif()
  endif()
  if(NOT EXISTS "${archive}")
    message(STATUS "Downloading ${filename}")
    file(DOWNLOAD "${WEBLOX_RELEASE_BASE}/${filename}" "${archive}"
      EXPECTED_HASH SHA256=${sha}
      TLS_VERIFY ON TIMEOUT 1200 SHOW_PROGRESS STATUS status)
    list(GET status 0 code)
    if(NOT code EQUAL 0)
      list(GET status 1 message)
      file(REMOVE "${archive}")
      message(FATAL_ERROR "Download of ${filename} failed: ${message}")
    endif()
  endif()

  if(filename MATCHES "\\.tar\\.zst$")
    get_filename_component(tarfile "${filename}" NAME_WE)
    set(tarfile "${WEBLOX_ASSET_DIR}/${tarfile}")
    execute_process(COMMAND "${ZSTD_EXECUTABLE}" -q -f -d "${archive}"
      -o "${tarfile}" RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "zstd failed on ${filename} (rc=${rc})")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E tar xf "${tarfile}"
      WORKING_DIRECTORY "${destination}" RESULT_VARIABLE rc)
    file(REMOVE "${tarfile}")
    if(NOT rc EQUAL 0)
      message(FATAL_ERROR "Extract of ${filename} failed (rc=${rc})")
    endif()
  else()
    configure_file("${archive}" "${destination}/${filename}" COPYONLY)
  endif()
  file(WRITE "${stamp}" "ok\n")
endfunction()
