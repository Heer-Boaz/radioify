set(RADIOIFY_LLAMA_REVISION 427291b5b34cd914a31b3fd3b61a68f6184f4b9f)
vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO ggml-org/llama.cpp
    REF ${RADIOIFY_LLAMA_REVISION}
    SHA512 eebe813a7d25926b499bd669c72fe45a1d4c6ad214b5fb7477f6348c33f49edebb65fc764ef9cf9e35685d29afdee2b242fb857a422d68e93475adc797b93c85
    HEAD_REF master
    PATCHES
        native-package.diff
)
file(REMOVE_RECURSE "${SOURCE_PATH}/ggml/include" "${SOURCE_PATH}/ggml/src")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DLLAMA_BUILD_COMMIT=${RADIOIFY_LLAMA_REVISION}
        -DLLAMA_BUILD_TOOLS=OFF
        -DLLAMA_OPENSSL=OFF
        -DGGML_CCACHE=OFF
        -DLLAMA_ALL_WARNINGS=OFF
        -DLLAMA_BUILD_TESTS=OFF
        -DLLAMA_BUILD_EXAMPLES=OFF
        -DLLAMA_BUILD_SERVER=OFF
        -DLLAMA_BUILD_APP=OFF
        -DLLAMA_BUILD_MTMD=ON
        -DLLAMA_BUILD_COMMON=OFF
        -DLLAMA_BUILD_IS_DEV=OFF
        # Radioify owns FFmpeg decoding, including stream selection and PTS.
        -DMTMD_VIDEO=OFF
        -DLLAMA_USE_SYSTEM_GGML=ON
        -DVCPKG_LOCK_FIND_PACKAGE_Git=OFF
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/llama")
vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()

file(INSTALL "${SOURCE_PATH}/gguf-py/gguf" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}/gguf-py")
file(INSTALL "${SOURCE_PATH}/convert_hf_to_gguf.py" DESTINATION "${CURRENT_PACKAGES_DIR}/tools/${PORT}" RENAME "convert-hf-to-gguf.py")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_clean_executables_in_bin(FILE_NAMES none)

set(gguf-py-license "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-rel/gguf-py LICENSE")
file(COPY_FILE "${SOURCE_PATH}/gguf-py/LICENSE" "${gguf-py-license}")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${gguf-py-license}")
