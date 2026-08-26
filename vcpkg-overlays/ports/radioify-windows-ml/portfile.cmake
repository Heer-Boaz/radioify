if(NOT VCPKG_TARGET_IS_WINDOWS OR NOT VCPKG_TARGET_ARCHITECTURE STREQUAL "x64")
  message(FATAL_ERROR "radioify-windows-ml currently supports Windows x64 only")
endif()

# Microsoft ships Windows ML as a self-contained NuGet runtime. Keep it
# dynamic even when Radioify itself uses the x64-windows-static triplet; the
# two Microsoft DLLs are deployed beside radioify.exe.
set(VCPKG_LIBRARY_LINKAGE dynamic)

vcpkg_download_distfile(ARCHIVE
  URLS "https://api.nuget.org/v3-flatcontainer/microsoft.windows.ai.machinelearning/2.2.12/microsoft.windows.ai.machinelearning.2.2.12.nupkg"
  FILENAME "microsoft.windows.ai.machinelearning.2.2.12.zip"
  SHA512 34566a68b3f357997f1ac9deef0a7684187c244e2724945fb931fa9b596abe0b3b229a66bdf75d2ca74664fedfd3ea666038f351a3520b425733c711b6f5e63e
)
vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE "${ARCHIVE}"
  NO_REMOVE_ONE_LEVEL)

file(INSTALL "${SOURCE_PATH}/include/winml"
     DESTINATION "${CURRENT_PACKAGES_DIR}/include")
foreach(CONFIG_ROOT IN ITEMS "" "debug/")
  file(INSTALL "${SOURCE_PATH}/lib/native/x64/onnxruntime.lib"
       DESTINATION "${CURRENT_PACKAGES_DIR}/${CONFIG_ROOT}lib")
  file(INSTALL
       "${SOURCE_PATH}/runtimes/win-x64/native/onnxruntime.dll"
       "${SOURCE_PATH}/runtimes/win-x64/native/DirectML.dll"
       DESTINATION "${CURRENT_PACKAGES_DIR}/${CONFIG_ROOT}bin")
endforeach()

file(INSTALL
     "${CMAKE_CURRENT_LIST_DIR}/radioify-windows-ml-config.cmake"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
file(INSTALL "${SOURCE_PATH}/ThirdPartyNotices.txt"
     DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/license.txt")
