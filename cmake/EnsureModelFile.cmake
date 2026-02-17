if (NOT DEFINED OUTPUT_PATH OR OUTPUT_PATH STREQUAL "")
  message(FATAL_ERROR "OUTPUT_PATH must be set.")
endif()
if (NOT DEFINED URL OR URL STREQUAL "")
  message(FATAL_ERROR "URL must be set.")
endif()
if (NOT DEFINED SHA256 OR SHA256 STREQUAL "")
  message(FATAL_ERROR "SHA256 must be set.")
endif()

string(TOLOWER "${SHA256}" _expected_hash)

get_filename_component(_output_dir "${OUTPUT_PATH}" DIRECTORY)
if (_output_dir)
  file(MAKE_DIRECTORY "${_output_dir}")
endif()

if (EXISTS "${OUTPUT_PATH}")
  file(SHA256 "${OUTPUT_PATH}" _existing_hash)
  string(TOLOWER "${_existing_hash}" _existing_hash)
  if (_existing_hash STREQUAL _expected_hash)
    message(STATUS "Neural model already present: ${OUTPUT_PATH}")
    return()
  endif()
  message(WARNING
    "Neural model hash mismatch at ${OUTPUT_PATH} "
    "(expected ${_expected_hash}, got ${_existing_hash}); re-downloading.")
  file(REMOVE "${OUTPUT_PATH}")
endif()

set(_download_tmp "${OUTPUT_PATH}.download")
file(REMOVE "${_download_tmp}")

file(DOWNLOAD
  "${URL}"
  "${_download_tmp}"
  STATUS _download_status
  SHOW_PROGRESS
  TLS_VERIFY ON
)
list(GET _download_status 0 _download_code)
list(GET _download_status 1 _download_message)
if (NOT _download_code EQUAL 0)
  file(REMOVE "${_download_tmp}")
  message(FATAL_ERROR
    "Failed to download neural model from ${URL} "
    "(code ${_download_code}): ${_download_message}")
endif()

file(SHA256 "${_download_tmp}" _downloaded_hash)
string(TOLOWER "${_downloaded_hash}" _downloaded_hash)
if (NOT _downloaded_hash STREQUAL _expected_hash)
  file(REMOVE "${_download_tmp}")
  message(FATAL_ERROR
    "Downloaded neural model hash mismatch for ${URL}: "
    "expected ${_expected_hash}, got ${_downloaded_hash}")
endif()

file(RENAME "${_download_tmp}" "${OUTPUT_PATH}")
message(STATUS "Downloaded neural model: ${OUTPUT_PATH}")
