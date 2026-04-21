set(RADIOIFY_WIN11_EXPLORER_ROOT "${CMAKE_BINARY_DIR}/win11-explorer")
set(RADIOIFY_WIN11_EXPLORER_BIN_DIR "${RADIOIFY_WIN11_EXPLORER_ROOT}/bin")
set(RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR "${RADIOIFY_WIN11_EXPLORER_ROOT}/package")
set(RADIOIFY_WIN11_EXPLORER_INCLUDE_DIR "${RADIOIFY_WIN11_EXPLORER_ROOT}/include")

file(MAKE_DIRECTORY "${RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR}")
file(MAKE_DIRECTORY "${RADIOIFY_WIN11_EXPLORER_INCLUDE_DIR}")

configure_file(
  "${CMAKE_SOURCE_DIR}/windows/package/Package.appxmanifest.in"
  "${RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR}/Package.appxmanifest"
  @ONLY
)
configure_file(
  "${CMAKE_SOURCE_DIR}/windows/explorer/radioify_explorer_config.h.in"
  "${RADIOIFY_WIN11_EXPLORER_INCLUDE_DIR}/radioify_explorer_config.h"
  @ONLY
)
configure_file(
  "${CMAKE_SOURCE_DIR}/windows/package/README.txt.in"
  "${RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR}/README.txt"
  @ONLY
)

add_library(radioify_explorer SHARED
  "${CMAKE_CURRENT_SOURCE_DIR}/radioify_explorer.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/radioify_explorer.def"
)
radioify_configure_repo_target(radioify_explorer)
target_include_directories(radioify_explorer PRIVATE "${RADIOIFY_WIN11_EXPLORER_INCLUDE_DIR}")
target_link_libraries(radioify_explorer PRIVATE ole32 shell32 shlwapi)
target_compile_definitions(radioify_explorer PRIVATE UNICODE _UNICODE)
set_target_properties(radioify_explorer PROPERTIES
  OUTPUT_NAME "radioify_explorer"
  RUNTIME_OUTPUT_DIRECTORY "${RADIOIFY_WIN11_EXPLORER_BIN_DIR}"
  LIBRARY_OUTPUT_DIRECTORY "${RADIOIFY_WIN11_EXPLORER_BIN_DIR}"
  ARCHIVE_OUTPUT_DIRECTORY "${RADIOIFY_WIN11_EXPLORER_BIN_DIR}"
)

foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(radioify_explorer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${RADIOIFY_WIN11_EXPLORER_BIN_DIR}/${config}"
    LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${RADIOIFY_WIN11_EXPLORER_BIN_DIR}/${config}"
    ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${RADIOIFY_WIN11_EXPLORER_BIN_DIR}/${config}"
  )
endforeach()

add_custom_target(radioify_win11_package_layout
  DEPENDS
    "${RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR}/Package.appxmanifest"
    "${RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR}/README.txt"
)

add_custom_target(radioify_win11_explorer_integration
  DEPENDS
    radioify
    radioify_explorer
    radioify_win11_package_layout
)
