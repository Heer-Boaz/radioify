vcpkg_from_bitbucket(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO multicoreware/x265_git
    REF "${VERSION}"
    SHA512 4b7d71f22f0a7f12ff93f9a01e361df2b80532cd8dac01b5465e63b5d8182f1a05c0289ad95f3aa972c963aa6cd90cb3d594f8b9a96f556a006cf7e1bdd9edda
    HEAD_REF master
    PATCHES
        disable-install-pdb.patch
        version.patch
        linkage.diff
        pkgconfig.diff
        pthread.diff
        compiler-target.diff
        neon.diff
        fix-cmake-4.patch
)

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        tool ENABLE_CLI
)

vcpkg_find_acquire_program(NASM)
vcpkg_find_acquire_program(NINJA)

set(COMMON_OPTIONS
    "-DNASM_EXECUTABLE=${NASM}"
    -DENABLE_PIC=ON
    "-DVERSION=${VERSION}"
)

# x265 chooses its internal output depth at compile time. Build a private
# Main10 library first, then link it into the public 8-bit API as prescribed by
# x265's multilib packaging contract. A normal 8-bit-only package cannot expose
# the Main10 API needed to retain source depth. Keep this private subbuild out
# of vcpkg-cmake's single public configure lifecycle.
function(build_x265_main10 BUILD_TYPE BUILD_DIR)
    file(REMOVE_RECURSE "${BUILD_DIR}")

    if(VCPKG_CRT_LINKAGE STREQUAL "static")
        set(MSVC_RUNTIME MultiThreaded)
    else()
        set(MSVC_RUNTIME MultiThreadedDLL)
    endif()
    if(BUILD_TYPE STREQUAL "Debug")
        if(VCPKG_CRT_LINKAGE STREQUAL "static")
            set(MSVC_RUNTIME MultiThreadedDebug)
        else()
            set(MSVC_RUNTIME MultiThreadedDebugDLL)
        endif()
    endif()

    string(TOLOWER "${BUILD_TYPE}" BUILD_TYPE_LOWER)
    vcpkg_execute_required_process(
        COMMAND
            "${CMAKE_COMMAND}"
            -S "${SOURCE_PATH}/source"
            -B "${BUILD_DIR}"
            -G Ninja
            "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
            "-DCMAKE_MAKE_PROGRAM=${NINJA}"
            -DCMAKE_POLICY_DEFAULT_CMP0091=NEW
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=${MSVC_RUNTIME}"
            ${COMMON_OPTIONS}
            -DHIGH_BIT_DEPTH=ON
            -DEXPORT_C_API=OFF
            -DENABLE_SHARED=OFF
            -DENABLE_CLI=OFF
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
        LOGNAME "configure-main10-${BUILD_TYPE_LOWER}-${TARGET_TRIPLET}"
    )
    vcpkg_execute_required_process(
        COMMAND
            "${CMAKE_COMMAND}"
            --build "${BUILD_DIR}"
            --target x265-static
        WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
        LOGNAME "build-main10-${BUILD_TYPE_LOWER}-${TARGET_TRIPLET}"
    )
endfunction()

set(MAIN10_RELEASE_DIR "${CURRENT_BUILDTREES_DIR}/main10-rel")
set(MAIN10_DEBUG_DIR "${CURRENT_BUILDTREES_DIR}/main10-dbg")
build_x265_main10(Release "${MAIN10_RELEASE_DIR}")
build_x265_main10(Debug "${MAIN10_DEBUG_DIR}")
set(MAIN10_RELEASE "${MAIN10_RELEASE_DIR}/x265-static.lib")
set(MAIN10_DEBUG "${MAIN10_DEBUG_DIR}/x265-static.lib")

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" ENABLE_SHARED)
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}/source"
    OPTIONS
        ${COMMON_OPTIONS}
        ${FEATURE_OPTIONS}
        "-DENABLE_SHARED=${ENABLE_SHARED}"
        -DLINKED_10BIT=ON
    OPTIONS_RELEASE
        "-DEXTRA_LIB=${MAIN10_RELEASE}"
    OPTIONS_DEBUG
        "-DEXTRA_LIB=${MAIN10_DEBUG}"
        -DENABLE_CLI=OFF
)
vcpkg_cmake_install()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "static")
    find_program(X265_LIBRARIAN NAMES lib.exe REQUIRED)

    function(merge_x265_depths CONFIGURATION PUBLIC_LIBRARY PRIVATE_LIBRARY)
        set(COMBINED_LIBRARY
            "${CURRENT_BUILDTREES_DIR}/x265-static-combined-${CONFIGURATION}.lib")
        vcpkg_execute_required_process(
            COMMAND
                "${X265_LIBRARIAN}"
                /NOLOGO
                /IGNORE:4006
                /IGNORE:4221
                "/OUT:${COMBINED_LIBRARY}"
                "${PUBLIC_LIBRARY}"
                "${PRIVATE_LIBRARY}"
            WORKING_DIRECTORY "${CURRENT_BUILDTREES_DIR}"
            LOGNAME "merge-multilib-${CONFIGURATION}-${TARGET_TRIPLET}"
        )
        file(REMOVE "${PUBLIC_LIBRARY}")
        file(RENAME "${COMBINED_LIBRARY}" "${PUBLIC_LIBRARY}")
    endfunction()

    merge_x265_depths(
        release
        "${CURRENT_PACKAGES_DIR}/lib/x265-static.lib"
        "${MAIN10_RELEASE}"
    )
    merge_x265_depths(
        debug
        "${CURRENT_PACKAGES_DIR}/debug/lib/x265-static.lib"
        "${MAIN10_DEBUG}"
    )
endif()

if("tool" IN_LIST FEATURES)
    vcpkg_copy_tools(TOOL_NAMES x265 AUTO_CLEAN)
endif()

if(VCPKG_TARGET_IS_WINDOWS AND VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
    vcpkg_replace_string(
        "${CURRENT_PACKAGES_DIR}/include/x265.h"
        "#ifdef X265_API_IMPORTS"
        "#if 1"
    )
endif()

vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING")
