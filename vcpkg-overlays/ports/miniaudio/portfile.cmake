vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO mackron/miniaudio
    REF "${VERSION}"
    SHA512 35f64a5888e9d83c6fb5bb3c0bcdce27499f021aa9118b64f03b5fa8b04736547d804d7e37acf9687b203e0e4282b4741971752e5d81f64e82e39a903e76baf6
    HEAD_REF master
    PATCHES cmake-package.diff
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DMINIAUDIO_BUILD_EXAMPLES=OFF
        -DMINIAUDIO_BUILD_TESTS=OFF
        -DMINIAUDIO_BUILD_TOOLS=OFF
        -DMINIAUDIO_NO_EXTRA_NODES=ON
        -DMINIAUDIO_NO_LIBVORBIS=ON
        -DMINIAUDIO_NO_LIBOPUS=ON
        -DMINIAUDIO_INSTALL=ON
)

vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH "lib/cmake/miniaudio")
vcpkg_fixup_pkgconfig()
vcpkg_copy_pdbs()

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
