set(RADIOIFY_WIN11_EXPLORER_ROOT "${CMAKE_BINARY_DIR}/win11-explorer")
set(RADIOIFY_WIN11_EXPLORER_BIN_DIR "${RADIOIFY_WIN11_EXPLORER_ROOT}/bin")
set(RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR "${RADIOIFY_WIN11_EXPLORER_ROOT}/package")
set(RADIOIFY_WIN11_EXPLORER_INCLUDE_DIR "${RADIOIFY_WIN11_EXPLORER_ROOT}/include")

file(MAKE_DIRECTORY "${RADIOIFY_WIN11_EXPLORER_PACKAGE_DIR}")
file(MAKE_DIRECTORY "${RADIOIFY_WIN11_EXPLORER_INCLUDE_DIR}")

set(RADIOIFY_WIN11_EXPLORER_FILE_TYPES
  .wav .mp3 .flac .m4a .webm .mp4 .mov .ogg .kss .nsf
  .mid .midi .vgm .vgz .psf .minipsf .psf2 .minipsf2
  .m4v .qt .mkv .avi .wmv .asf .flv .mpg .mpeg .mpe
  .mpv .m2v .ts .m2ts .mts .3gp .3g2 .ogv .vob
  .jpg .jpeg .png .bmp
)
if (NOT RADIOIFY_DISABLE_GSF_GPL)
  list(APPEND RADIOIFY_WIN11_EXPLORER_FILE_TYPES .gsf .minigsf)
endif()
list(REMOVE_DUPLICATES RADIOIFY_WIN11_EXPLORER_FILE_TYPES)

set(RADIOIFY_WIN11_EXPLORER_FILE_TYPE_ASSOCIATION_XML
"        <uap:Extension Category=\"windows.fileTypeAssociation\" EntryPoint=\"Windows.FullTrustApplication\">\n"
)
string(APPEND RADIOIFY_WIN11_EXPLORER_FILE_TYPE_ASSOCIATION_XML
"          <uap:FileTypeAssociation Name=\"radioifymedia\">\n"
"            <uap:DisplayName>Radioify media</uap:DisplayName>\n"
"            <uap:Logo>Assets\\SmallLogo.png</uap:Logo>\n"
"            <uap:SupportedFileTypes>\n"
)
foreach(radioify_file_type IN LISTS RADIOIFY_WIN11_EXPLORER_FILE_TYPES)
  string(APPEND RADIOIFY_WIN11_EXPLORER_FILE_TYPE_ASSOCIATION_XML
"              <uap:FileType>${radioify_file_type}</uap:FileType>\n"
)
endforeach()
string(APPEND RADIOIFY_WIN11_EXPLORER_FILE_TYPE_ASSOCIATION_XML
"            </uap:SupportedFileTypes>\n"
"            <desktop2:ThumbnailHandler Clsid=\"${RADIOIFY_WIN11_EXPLORER_THUMBNAIL_GUID}\" />\n"
"            <uap2:SupportedVerbs>\n"
"              <uap3:Verb Id=\"RadioifyOpenFile\" Parameters=\"--shell-open &quot;%1&quot;\">Open with Radioify</uap3:Verb>\n"
"            </uap2:SupportedVerbs>\n"
"          </uap:FileTypeAssociation>\n"
"        </uap:Extension>"
)

set(RADIOIFY_WIN11_EXPLORER_CONTEXT_MENU_XML
"        <desktop4:Extension Category=\"windows.fileExplorerContextMenus\">\n          <desktop4:FileExplorerContextMenus>\n"
)
foreach(radioify_file_type IN LISTS RADIOIFY_WIN11_EXPLORER_FILE_TYPES)
  string(SUBSTRING "${radioify_file_type}" 1 -1 radioify_file_type_id)
  string(REGEX REPLACE "[^A-Za-z0-9]" "" radioify_file_type_id "${radioify_file_type_id}")
  string(APPEND RADIOIFY_WIN11_EXPLORER_CONTEXT_MENU_XML
"            <desktop5:ItemType Type=\"${radioify_file_type}\">\n"
"              <desktop5:Verb Id=\"RadioifyOpen${radioify_file_type_id}\" Clsid=\"${RADIOIFY_WIN11_EXPLORER_COMMAND_GUID}\" />\n"
"            </desktop5:ItemType>\n"
)
endforeach()
string(APPEND RADIOIFY_WIN11_EXPLORER_CONTEXT_MENU_XML
"            <desktop5:ItemType Type=\"Directory\">\n"
"              <desktop5:Verb Id=\"RadioifyOpen\" Clsid=\"${RADIOIFY_WIN11_EXPLORER_COMMAND_GUID}\" />\n"
"            </desktop5:ItemType>\n"
"            <desktop5:ItemType Type=\"Directory\\Background\">\n"
"              <desktop5:Verb Id=\"RadioifyOpenBackground\" Clsid=\"${RADIOIFY_WIN11_EXPLORER_COMMAND_GUID}\" />\n"
"            </desktop5:ItemType>\n"
"          </desktop4:FileExplorerContextMenus>\n"
"        </desktop4:Extension>"
)

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
if (FFMPEG_LIBRARY_DIRS)
  target_link_directories(radioify_explorer PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(radioify_explorer PRIVATE gdi32 ole32 shell32 shlwapi windowscodecs ${FFMPEG_LIBRARIES})
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
