set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)

if(PORT STREQUAL "aom")
    set(VCPKG_ENV_PASSTHROUGH
        RADIOIFY_AOM_CLANG_CL
        RADIOIFY_AOM_RC
        RADIOIFY_VCPKG_ROOT
    )
    set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
        "${CMAKE_CURRENT_LIST_DIR}/../toolchains/windows-aom-clang-cl.cmake"
    )
endif()
