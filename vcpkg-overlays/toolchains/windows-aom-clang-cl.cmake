if(NOT DEFINED ENV{RADIOIFY_VCPKG_ROOT})
    message(FATAL_ERROR "The Radioify AOM build requires the vcpkg root.")
endif()

file(TO_CMAKE_PATH "$ENV{RADIOIFY_VCPKG_ROOT}" AOM_VCPKG_ROOT)
include("${AOM_VCPKG_ROOT}/scripts/toolchains/windows.cmake")

if(NOT DEFINED ENV{RADIOIFY_AOM_CLANG_CL} OR
   NOT DEFINED ENV{RADIOIFY_AOM_RC})
    message(FATAL_ERROR
        "The Radioify AOM build requires clang-cl and the Windows SDK rc.exe."
    )
endif()

set(AOM_CLANG_CL "$ENV{RADIOIFY_AOM_CLANG_CL}")
execute_process(
    COMMAND "${AOM_CLANG_CL}" --version
    RESULT_VARIABLE AOM_CLANG_RESULT
    OUTPUT_VARIABLE AOM_CLANG_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT AOM_CLANG_RESULT EQUAL 0 OR
   NOT AOM_CLANG_VERSION MATCHES "clang version ([0-9]+)" OR
   CMAKE_MATCH_1 LESS 19)
    message(FATAL_ERROR
        "AOM requires clang-cl 19 or newer on Windows; found: ${AOM_CLANG_VERSION}"
    )
endif()

# MSVC 19.44 miscompiles an unaligned stack spill in motion_mode_rd and
# crashes valid 10-bit AV1 encodes. Keep AOM on Radioify's clang-cl ABI instead
# of patching codec source or disabling encoder algorithms.
set(CMAKE_C_COMPILER "${AOM_CLANG_CL}" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${AOM_CLANG_CL}" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "$ENV{RADIOIFY_AOM_RC}" CACHE FILEPATH "" FORCE)
