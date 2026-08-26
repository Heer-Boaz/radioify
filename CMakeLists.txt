cmake_minimum_required(VERSION 3.21)
if (POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(radioify LANGUAGES C CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

function(radioify_enable_strict_warnings target_name)
  if (MSVC)
    target_compile_options(${target_name} PRIVATE
      $<$<COMPILE_LANGUAGE:C>:/W4>
      $<$<COMPILE_LANGUAGE:C>:/we4100>
      $<$<COMPILE_LANGUAGE:C>:/we4101>
      $<$<COMPILE_LANGUAGE:C>:/we4189>
      $<$<COMPILE_LANGUAGE:C>:/we4505>
      $<$<COMPILE_LANGUAGE:CXX>:/W4>
      $<$<COMPILE_LANGUAGE:CXX>:/we4100>
      $<$<COMPILE_LANGUAGE:CXX>:/we4101>
      $<$<COMPILE_LANGUAGE:CXX>:/we4189>
      $<$<COMPILE_LANGUAGE:CXX>:/we4505>
    )
  else()
    target_compile_options(${target_name} PRIVATE
      $<$<COMPILE_LANGUAGE:C>:-Wall>
      $<$<COMPILE_LANGUAGE:C>:-Wextra>
      $<$<COMPILE_LANGUAGE:C>:-Wpedantic>
      $<$<COMPILE_LANGUAGE:C>:-Wunused-parameter>
      $<$<COMPILE_LANGUAGE:C>:-Wunused-variable>
      $<$<COMPILE_LANGUAGE:C>:-Wunused-function>
      $<$<COMPILE_LANGUAGE:C>:-Werror=unused-parameter>
      $<$<COMPILE_LANGUAGE:C>:-Werror=unused-variable>
      $<$<COMPILE_LANGUAGE:C>:-Werror=unused-function>
      $<$<COMPILE_LANGUAGE:CXX>:-Wall>
      $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
      $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
      $<$<COMPILE_LANGUAGE:CXX>:-Wunused-parameter>
      $<$<COMPILE_LANGUAGE:CXX>:-Wunused-variable>
      $<$<COMPILE_LANGUAGE:CXX>:-Wunused-function>
      $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-parameter>
      $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-variable>
      $<$<COMPILE_LANGUAGE:CXX>:-Werror=unused-function>
    )
  endif()
endfunction()

function(radioify_disable_warnings target_name)
  if (MSVC)
    target_compile_options(${target_name} PRIVATE
      $<$<COMPILE_LANGUAGE:C>:/W0>
      $<$<COMPILE_LANGUAGE:CXX>:/W0>
      $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    )
    if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
      target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:/clang:-Wno-unknown-argument>
        $<$<COMPILE_LANGUAGE:CXX>:/clang:-Wno-unknown-argument>
        $<$<COMPILE_LANGUAGE:C>:-Wno-unknown-argument>
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-unknown-argument>
      )
    endif()
  else()
    target_compile_options(${target_name} PRIVATE
      $<$<COMPILE_LANGUAGE:C>:-w>
      $<$<COMPILE_LANGUAGE:CXX>:-w>
    )
  endif()
endfunction()

function(radioify_add_external_include_dirs)
  if (ARGN)
    target_include_directories(radioify_external_options SYSTEM INTERFACE ${ARGN})
  endif()
endfunction()

function(radioify_link_repo_target target_name)
  target_link_libraries(${target_name} PRIVATE
    radioify_project_options
    radioify_external_options
  )
endfunction()

function(radioify_configure_repo_target target_name)
  radioify_link_repo_target(${target_name})
  radioify_enable_strict_warnings(${target_name})
endfunction()

function(radioify_configure_external_target target_name)
  radioify_disable_warnings(${target_name})
endfunction()

if (MSVC)
  set(_radioify_msvc_runtime "")
  if (DEFINED VCPKG_TARGET_TRIPLET)
    set(_radioify_vcpkg_triplet_runtime_probe "${VCPKG_TARGET_TRIPLET}")
  elseif (DEFINED ENV{VCPKG_TARGET_TRIPLET})
    set(_radioify_vcpkg_triplet_runtime_probe "$ENV{VCPKG_TARGET_TRIPLET}")
  else()
    set(_radioify_vcpkg_triplet_runtime_probe "")
  endif()

  if (_radioify_vcpkg_triplet_runtime_probe MATCHES "-static-md$")
    set(_radioify_msvc_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
  elseif (_radioify_vcpkg_triplet_runtime_probe MATCHES "-static$")
    set(_radioify_msvc_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  if (_radioify_msvc_runtime)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "${_radioify_msvc_runtime}" CACHE STRING "MSVC runtime" FORCE)
    message(STATUS "MSVC runtime forced to ${CMAKE_MSVC_RUNTIME_LIBRARY} for triplet ${_radioify_vcpkg_triplet_runtime_probe}")
  endif()
endif()

option(RADIOIFY_ENABLE_TIMING_LOG "Enable timing log output" OFF)
option(RADIOIFY_ENABLE_VIDEO_ERROR_LOG "Enable video error log output" OFF)
option(RADIOIFY_ENABLE_FFMPEG_ERROR_LOG "Enable ffmpeg error log output" OFF)
option(RADIOIFY_ENABLE_STAGING_UPLOAD "Enable staging-based texture uploads to avoid implicit GPU sync" OFF)
option(RADIOIFY_ENABLE_MELODY_ANALYSIS "Enable offline melody analysis (ONNX backend)" OFF)
option(RADIOIFY_ENABLE_NEURAL_PITCH "Deprecated alias for RADIOIFY_ENABLE_MELODY_ANALYSIS" OFF)
option(RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_PROCESSOR "Enable NVIDIA D3D11 video processor RTX enhancement integration" ON)
option(RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK "Enable NVIDIA RTX Video SDK/NGX integration" OFF)
option(RADIOIFY_BUILD_WIN11_EXPLORER_INTEGRATION "Build the Windows 11 Explorer integration scaffold" OFF)
option(RADIOIFY_DOWNLOAD_NEURAL_PITCH_MODEL "Download neural pitch model automatically when missing" ON)
option(RADIOIFY_DOWNLOAD_WHISPER_MODEL "Download the indexed-transcript Whisper model automatically when missing" ON)
set(_radioify_audio_separation_default OFF)
if (WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(_radioify_audio_separation_default ON)
endif()
option(RADIOIFY_ENABLE_AUDIO_SEPARATION
       "Enable DirectML dialogue/music/effects separation"
       ${_radioify_audio_separation_default})
set(RADIOIFY_HAS_AUDIO_SEPARATION OFF)
if (RADIOIFY_ENABLE_AUDIO_SEPARATION)
  if (NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR
      "RADIOIFY_ENABLE_AUDIO_SEPARATION currently requires Windows x64.")
  endif()
  set(RADIOIFY_HAS_AUDIO_SEPARATION ON)
endif()
set(RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT "" CACHE PATH
    "NVIDIA RTX Video SDK/NGX root containing include/ and lib/x64/")
if (WIN32)
  set(RADIOIFY_WIN11_EXPLORER_PACKAGE_NAME "Radioify.Win11Explorer")
  set(RADIOIFY_WIN11_EXPLORER_PACKAGE_PUBLISHER "CN=Radioify-Development")
  string(TIMESTAMP _radioify_win11_package_year "%Y")
  string(TIMESTAMP _radioify_win11_package_month "%m")
  string(TIMESTAMP _radioify_win11_package_day "%d")
  string(TIMESTAMP _radioify_win11_package_hour "%H")
  string(TIMESTAMP _radioify_win11_package_minute "%M")
  string(TIMESTAMP _radioify_win11_package_second "%S")
  foreach(_radioify_win11_package_part IN ITEMS
      month day hour minute second)
    string(REGEX REPLACE "^0+" "" _radioify_win11_package_${_radioify_win11_package_part}
      "${_radioify_win11_package_${_radioify_win11_package_part}}")
    if ("${_radioify_win11_package_${_radioify_win11_package_part}}" STREQUAL "")
      set(_radioify_win11_package_${_radioify_win11_package_part} "0")
    endif()
  endforeach()
  math(EXPR _radioify_win11_package_mmdd
    "${_radioify_win11_package_month} * 100 + ${_radioify_win11_package_day}")
  math(EXPR _radioify_win11_package_hhmm
    "${_radioify_win11_package_hour} * 100 + ${_radioify_win11_package_minute}")
  set(RADIOIFY_WIN11_EXPLORER_PACKAGE_VERSION
    "${_radioify_win11_package_year}.${_radioify_win11_package_mmdd}.${_radioify_win11_package_hhmm}.${_radioify_win11_package_second}")
  set(RADIOIFY_WIN11_EXPLORER_APPLICATION_ID "RadioifyExplorerStub")
  set(RADIOIFY_WIN11_EXPLORER_COMMAND_GUID "4A616892-7E56-4D0B-BD0E-6A2E4F7B0E91")
  set(RADIOIFY_WIN11_EXPLORER_COMMAND_CLSID "{${RADIOIFY_WIN11_EXPLORER_COMMAND_GUID}}")
  set(RADIOIFY_WIN11_EXPLORER_NON_ASCII_COMMAND_GUID "99A29345-98F4-4435-BF03-E68187F76EFF")
  set(RADIOIFY_WIN11_EXPLORER_NON_ASCII_COMMAND_CLSID "{${RADIOIFY_WIN11_EXPLORER_NON_ASCII_COMMAND_GUID}}")
  set(RADIOIFY_WIN11_EXPLORER_THUMBNAIL_GUID "A1082EA3-4A99-451F-89FE-1C72011133B8")
  set(RADIOIFY_WIN11_EXPLORER_THUMBNAIL_CLSID "{${RADIOIFY_WIN11_EXPLORER_THUMBNAIL_GUID}}")
  set(RADIOIFY_WIN11_EXPLORER_COMMAND_APPID
    "${RADIOIFY_WIN11_EXPLORER_COMMAND_GUID}")
  set(RADIOIFY_WIN11_EXPLORER_THUMBNAIL_APPID
    "${RADIOIFY_WIN11_EXPLORER_THUMBNAIL_GUID}")
  if (CMAKE_SYSTEM_PROCESSOR MATCHES "^[Aa][Rr][Mm]64$|^[Aa][Aa][Rr][Cc][Hh]64$")
    set(RADIOIFY_WIN11_EXPLORER_PACKAGE_ARCH "arm64")
  elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(RADIOIFY_WIN11_EXPLORER_PACKAGE_ARCH "x64")
  else()
    set(RADIOIFY_WIN11_EXPLORER_PACKAGE_ARCH "x86")
  endif()
endif()
set(RADIOIFY_ENABLE_MELODY_ANALYSIS_EFFECTIVE OFF)
if (RADIOIFY_ENABLE_MELODY_ANALYSIS OR RADIOIFY_ENABLE_NEURAL_PITCH)
  set(RADIOIFY_ENABLE_MELODY_ANALYSIS_EFFECTIVE ON)
endif()
if (RADIOIFY_ENABLE_NEURAL_PITCH AND NOT RADIOIFY_ENABLE_MELODY_ANALYSIS)
  message(WARNING "RADIOIFY_ENABLE_NEURAL_PITCH is deprecated; use RADIOIFY_ENABLE_MELODY_ANALYSIS instead.")
endif()
set(RADIOIFY_NEURAL_PITCH_MODEL_PATH "${CMAKE_SOURCE_DIR}/models/crepe_small.onnx" CACHE FILEPATH
    "Path to the offline neural pitch ONNX model")
set(RADIOIFY_NEURAL_PITCH_MODEL_URL
    "https://huggingface.co/wok000/weights/resolve/main/crepe/onnx/small.onnx"
    CACHE STRING "URL used to download the default neural pitch ONNX model")
set(RADIOIFY_NEURAL_PITCH_MODEL_SHA256
    "cd119f6f5f608d9342a1c6f81e4653f21dbcde440a96d474ecbf6f27c6253466"
    CACHE STRING "Expected SHA256 hash for the downloaded neural pitch ONNX model")
set(RADIOIFY_WHISPER_MODEL_PATH
    "${CMAKE_SOURCE_DIR}/models/ggml-base-q5_1.bin" CACHE FILEPATH
    "Path to the multilingual Whisper model used for indexed transcripts")
set(RADIOIFY_WHISPER_MODEL_URL
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base-q5_1.bin"
    CACHE STRING "URL used to download the indexed-transcript Whisper model")
set(RADIOIFY_WHISPER_MODEL_SHA256
    "422f1ae452ade6f30a004d7e5c6a43195e4433bc370bf23fac9cc591f01a8898"
    CACHE STRING "Expected SHA256 hash for the indexed-transcript Whisper model")
set(RADIOIFY_AUDIO_SEPARATION_MODEL_PATH
    "${CMAKE_SOURCE_DIR}/models/audio_separation/bandit-v2-multi-mask-core-fp16.onnx"
    CACHE FILEPATH "Path to Radioify's BandIt v2 mask model")
set(RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256
    "4694632688ae0b9a3e4c46918c0f9bb4f2af18c393d8e7d55f3e1e276d6fa6c7")

set(RADIOIFY_PROJECT_INCLUDE_DIRS
  ${CMAKE_SOURCE_DIR}/app
  ${CMAKE_SOURCE_DIR}/audio
  ${CMAKE_SOURCE_DIR}/audio/analysis
  ${CMAKE_SOURCE_DIR}/audio/audiofilter
  ${CMAKE_SOURCE_DIR}/audio/audiofilter/config
  ${CMAKE_SOURCE_DIR}/audio/loopsplit
  ${CMAKE_SOURCE_DIR}/core
  ${CMAKE_SOURCE_DIR}/tui
  ${CMAKE_SOURCE_DIR}/tui/ui
  ${CMAKE_SOURCE_DIR}/playback/ascii
  ${CMAKE_SOURCE_DIR}/playback/framebuffer
  ${CMAKE_SOURCE_DIR}/playback/video
  ${CMAKE_SOURCE_DIR}/playback/video/ascii
  ${CMAKE_SOURCE_DIR}/playback/video/framebuffer
  ${CMAKE_SOURCE_DIR}/playback/video/gpu
)

add_library(radioify_project_options INTERFACE)
target_include_directories(radioify_project_options INTERFACE
  ${CMAKE_SOURCE_DIR}
  ${RADIOIFY_PROJECT_INCLUDE_DIRS}
  ${CMAKE_SOURCE_DIR}/playback/session
  ${CMAKE_SOURCE_DIR}/playback/overlay
)
add_library(radioify_external_options INTERFACE)
target_include_directories(radioify_external_options SYSTEM INTERFACE
  ${CMAKE_SOURCE_DIR}/third_party
)

set(RADIOIFY_HAS_NVIDIA_RTX_VIDEO_SDK OFF)
if (RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK AND
    NOT RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_PROCESSOR)
  message(FATAL_ERROR
    "RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK requires "
    "RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_PROCESSOR=ON.")
endif()

if (RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK)
  set(_radioify_nvidia_rtx_video_sdk_hints "")
  if (RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT)
    list(APPEND _radioify_nvidia_rtx_video_sdk_hints
         "${RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT}")
  endif()
  foreach(_radioify_nvidia_rtx_video_sdk_env IN ITEMS
      RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT
      NVIDIA_RTX_VIDEO_SDK_ROOT
      RTX_VIDEO_SDK_ROOT
      NGX_SDK_ROOT
      NGX_SDK_DIR)
    if (DEFINED ENV{${_radioify_nvidia_rtx_video_sdk_env}})
      list(APPEND _radioify_nvidia_rtx_video_sdk_hints
           "$ENV{${_radioify_nvidia_rtx_video_sdk_env}}")
    endif()
  endforeach()
  list(APPEND _radioify_nvidia_rtx_video_sdk_hints
       "C:/ProgramData/NVIDIA Corporation/NGX SDK"
       "C:/Program Files/NVIDIA Corporation/RTX Video SDK"
       "C:/Program Files/NVIDIA Corporation/NGX SDK"
       "C:/Program Files (x86)/NVIDIA Corporation/NGX SDK")

  set(_radioify_nvidia_rtx_video_sdk_include_hints "")
  set(_radioify_nvidia_rtx_video_sdk_lib_hints "")
  foreach(_radioify_nvidia_rtx_video_sdk_hint IN LISTS _radioify_nvidia_rtx_video_sdk_hints)
    list(APPEND _radioify_nvidia_rtx_video_sdk_include_hints
         "${_radioify_nvidia_rtx_video_sdk_hint}/include")
    list(APPEND _radioify_nvidia_rtx_video_sdk_lib_hints
         "${_radioify_nvidia_rtx_video_sdk_hint}/lib/x64")
  endforeach()

  find_path(RADIOIFY_NVIDIA_RTX_VIDEO_SDK_INCLUDE_DIR
            NAMES nvsdk_ngx.h
            PATHS ${_radioify_nvidia_rtx_video_sdk_include_hints}
            NO_DEFAULT_PATH)
  find_file(RADIOIFY_NVIDIA_RTX_VIDEO_SDK_DEFS_HEADER
            NAMES nvsdk_ngx_defs.h
            PATHS ${_radioify_nvidia_rtx_video_sdk_include_hints}
            NO_DEFAULT_PATH)
  find_library(RADIOIFY_NVIDIA_RTX_VIDEO_SDK_LIBRARY
               NAMES nvsdk_ngx_s nvsdk_ngx_d
               PATHS ${_radioify_nvidia_rtx_video_sdk_lib_hints}
               NO_DEFAULT_PATH)

  if (RADIOIFY_NVIDIA_RTX_VIDEO_SDK_INCLUDE_DIR AND
      RADIOIFY_NVIDIA_RTX_VIDEO_SDK_DEFS_HEADER AND
      RADIOIFY_NVIDIA_RTX_VIDEO_SDK_LIBRARY)
    set(RADIOIFY_HAS_NVIDIA_RTX_VIDEO_SDK ON)
    get_filename_component(RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT_RESOLVED
      "${RADIOIFY_NVIDIA_RTX_VIDEO_SDK_INCLUDE_DIR}/.." ABSOLUTE)
    radioify_add_external_include_dirs(
      "${RADIOIFY_NVIDIA_RTX_VIDEO_SDK_INCLUDE_DIR}")
    target_link_libraries(radioify_project_options INTERFACE
      "${RADIOIFY_NVIDIA_RTX_VIDEO_SDK_LIBRARY}")
    message(STATUS
      "NVIDIA RTX Video SDK found: ${RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT_RESOLVED}")
  else()
    message(FATAL_ERROR
      "RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK=ON, but NVIDIA RTX Video SDK/NGX was not found. "
      "Run .\\build.ps1 -Static -InstallDeps after downloading the SDK from "
      "https://developer.nvidia.com/rtx-video-sdk, set RADIOIFY_NVIDIA_RTX_VIDEO_SDK_ROOT, "
      "or configure with RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK=OFF.")
  endif()
endif()

target_compile_definitions(radioify_project_options INTERFACE
  RADIOIFY_ENABLE_TIMING_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_TIMING_LOG}>,1,0>
  RADIOIFY_ENABLE_VIDEO_ERROR_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_VIDEO_ERROR_LOG}>,1,0>
  RADIOIFY_ENABLE_FFMPEG_ERROR_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_FFMPEG_ERROR_LOG}>,1,0>
  RADIOIFY_ENABLE_STAGING_UPLOAD=$<IF:$<BOOL:${RADIOIFY_ENABLE_STAGING_UPLOAD}>,1,0>
  RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK=$<IF:$<BOOL:${RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK}>,1,0>
  RADIOIFY_HAS_NVIDIA_RTX_VIDEO_SDK=$<IF:$<BOOL:${RADIOIFY_HAS_NVIDIA_RTX_VIDEO_SDK}>,1,0>
  RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO=$<IF:$<BOOL:${RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_PROCESSOR}>,1,0>
  RADIOIFY_HAS_AUDIO_SEPARATION=$<IF:$<BOOL:${RADIOIFY_HAS_AUDIO_SEPARATION}>,1,0>
)
if (MSVC)
  target_compile_options(radioify_project_options INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
  )
  target_compile_options(radioify_external_options INTERFACE
    $<$<COMPILE_LANGUAGE:C>:/external:W0>
    $<$<COMPILE_LANGUAGE:CXX>:/external:W0>
  )
endif()

function(radioify_add_module_object_library target_name)
  add_library(${target_name} OBJECT ${ARGN})
  radioify_configure_repo_target(${target_name})
endfunction()

add_subdirectory(app)
add_subdirectory(audio)
add_subdirectory(core)
add_subdirectory(playback)
add_subdirectory(tools)
add_subdirectory(tui)

if (RADIOIFY_HAS_AUDIO_SEPARATION)
  find_package(radioify-windows-ml CONFIG REQUIRED)
  target_link_libraries(radioify_audio_separation PRIVATE
    WindowsML::OnnxRuntime)

  if (NOT EXISTS "${RADIOIFY_AUDIO_SEPARATION_MODEL_PATH}")
    message(FATAL_ERROR
      "BandIt v2 model not found at ${RADIOIFY_AUDIO_SEPARATION_MODEL_PATH}. "
      "Ensure Git LFS objects were fetched.")
  endif()
  file(SIZE "${RADIOIFY_AUDIO_SEPARATION_MODEL_PATH}"
       _radioify_audio_separation_model_size)
  if (_radioify_audio_separation_model_size LESS 1000000)
    message(FATAL_ERROR
      "BandIt v2 model is too small and is probably a Git LFS pointer: "
      "${RADIOIFY_AUDIO_SEPARATION_MODEL_PATH}")
  endif()
  file(SHA256 "${RADIOIFY_AUDIO_SEPARATION_MODEL_PATH}"
       _radioify_audio_separation_model_hash)
  if (NOT _radioify_audio_separation_model_hash STREQUAL
      RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256)
    message(FATAL_ERROR
      "BandIt v2 model hash mismatch. Expected "
      "${RADIOIFY_AUDIO_SEPARATION_MODEL_SHA256}, got "
      "${_radioify_audio_separation_model_hash}.")
  endif()

  function(radioify_deploy_windows_ml target_name)
    add_custom_command(TARGET ${target_name} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "$<TARGET_FILE:WindowsML::OnnxRuntime>"
              "$<TARGET_FILE_DIR:${target_name}>/onnxruntime.dll"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${WINML_DIRECTML_DLL}"
              "$<TARGET_FILE_DIR:${target_name}>/DirectML.dll"
      COMMENT "Publishing self-contained Windows ML runtime for ${target_name}"
      VERBATIM
    )
  endfunction()
endif()

add_executable(radioify)
if (WIN32)
  set_source_files_properties("${CMAKE_SOURCE_DIR}/app/radioify.rc"
    PROPERTIES
      OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/radioify.ico;${CMAKE_SOURCE_DIR}/app/radioify.exe.manifest"
  )
endif()
target_sources(radioify PRIVATE
  $<$<PLATFORM_ID:Windows>:${CMAKE_SOURCE_DIR}/app/radioify.rc>
  $<TARGET_OBJECTS:radioify_app>
  $<TARGET_OBJECTS:radioify_tui>
  $<TARGET_OBJECTS:radioify_video_playback>
  $<TARGET_OBJECTS:radioify_timeline_preview>
  $<TARGET_OBJECTS:radioify_video_sidecar_identity>
  $<TARGET_OBJECTS:radioify_transcript_artifact>
  $<TARGET_OBJECTS:radioify_transcript_document>
  $<TARGET_OBJECTS:radioify_transcript_cue_semantics>
  $<TARGET_OBJECTS:radioify_transcript_device_policy>
  $<TARGET_OBJECTS:radioify_transcript_job>
  $<TARGET_OBJECTS:radioify_transcript_engine>
  $<TARGET_OBJECTS:radioify_scene_analysis>
  $<TARGET_OBJECTS:radioify_image_codec>
  $<TARGET_OBJECTS:radioify_video>
  $<TARGET_OBJECTS:radioify_audio>
  $<TARGET_OBJECTS:radioify_audio_separation>
  $<TARGET_OBJECTS:radioify_analysis>
  $<TARGET_OBJECTS:radioify_ui>
  $<TARGET_OBJECTS:radioify_core_runtime>
  $<TARGET_OBJECTS:radioify_core>
  $<TARGET_OBJECTS:radioify_playback>
  $<TARGET_OBJECTS:radioify_config>
  $<TARGET_OBJECTS:radioify_audiofilter>
  $<TARGET_OBJECTS:radioify_loopsplit>
)
if (MSVC)
  target_link_options(radioify PRIVATE /MANIFEST:NO)
endif()
radioify_link_repo_target(radioify)
if (RADIOIFY_HAS_AUDIO_SEPARATION)
  target_link_libraries(radioify PRIVATE WindowsML::OnnxRuntime)
  radioify_deploy_windows_ml(radioify)
endif()

add_executable(radio_measurements)
target_sources(radio_measurements PRIVATE
  $<TARGET_OBJECTS:radioify_tool_radio_measurements>
  $<TARGET_OBJECTS:radioify_config>
  $<TARGET_OBJECTS:radioify_audiofilter>
)
radioify_link_repo_target(radio_measurements)
add_test(NAME radio_measurements_philco_reference
         COMMAND $<TARGET_FILE:radio_measurements>
                 --preset philco-37-116 --section reference)
add_test(NAME radio_measurements_typical_reference
         COMMAND $<TARGET_FILE:radio_measurements>
                 --preset typical-1930s --section reference)

add_executable(radio_node_physics)
target_sources(radio_node_physics PRIVATE
  $<TARGET_OBJECTS:radioify_tool_radio_node_physics>
  $<TARGET_OBJECTS:radioify_config>
  $<TARGET_OBJECTS:radioify_audiofilter>
)
radioify_link_repo_target(radio_node_physics)
add_test(NAME radio_node_physics_philco
         COMMAND $<TARGET_FILE:radio_node_physics>
                 --preset philco-37-116)
add_test(NAME radio_node_physics_typical
         COMMAND $<TARGET_FILE:radio_node_physics>
                 --preset typical-1930s)

add_executable(radio_receiver_profile_tests)
target_sources(radio_receiver_profile_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_radio_receiver_profile_tests>
  $<TARGET_OBJECTS:radioify_config>
  $<TARGET_OBJECTS:radioify_audiofilter>
  ${CMAKE_SOURCE_DIR}/audio/pipeline_transition.cpp
  ${CMAKE_SOURCE_DIR}/audio/radio_filter_mode.cpp
  ${CMAKE_SOURCE_DIR}/audio/radio_filter_transition.cpp
  ${CMAKE_SOURCE_DIR}/audio/radio_playback.cpp
)
radioify_link_repo_target(radio_receiver_profile_tests)
add_test(NAME radio_receiver_profile_tests
         COMMAND $<TARGET_FILE:radio_receiver_profile_tests>)

add_executable(radio_am_ingress_tests)
target_sources(radio_am_ingress_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_radio_am_ingress_tests>
  $<TARGET_OBJECTS:radioify_config>
  $<TARGET_OBJECTS:radioify_audiofilter>
)
radioify_link_repo_target(radio_am_ingress_tests)
add_test(NAME radio_am_ingress_tests
         COMMAND $<TARGET_FILE:radio_am_ingress_tests>)

add_executable(audio_playback_pipeline_tests)
target_sources(audio_playback_pipeline_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_audio_playback_pipeline_tests>
  ${CMAKE_SOURCE_DIR}/audio/pipeline_transition.cpp
  ${CMAKE_SOURCE_DIR}/audio/playback_source_priming.cpp
  ${CMAKE_SOURCE_DIR}/audio/radio_filter_mode.cpp
  ${CMAKE_SOURCE_DIR}/audio/radio_filter_transition.cpp
  ${CMAKE_SOURCE_DIR}/audio/seek_presentation.cpp
  ${CMAKE_SOURCE_DIR}/audio/spsc_audio_ring.cpp
  ${CMAKE_SOURCE_DIR}/audio/spsc_audio_timeline.cpp
)
radioify_link_repo_target(audio_playback_pipeline_tests)
add_test(NAME audio_playback_pipeline_tests
         COMMAND $<TARGET_FILE:audio_playback_pipeline_tests>)

add_executable(audio_media_format_tests)
target_sources(audio_media_format_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_audio_media_format_tests>
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
)
radioify_link_repo_target(audio_media_format_tests)
add_test(NAME audio_media_format_tests
         COMMAND $<TARGET_FILE:audio_media_format_tests>)

add_executable(ffmpeg_audio_decoder_tests)
target_sources(ffmpeg_audio_decoder_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_ffmpeg_audio_decoder_tests>
  ${CMAKE_SOURCE_DIR}/audio/ffmpegaudio.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
)
radioify_link_repo_target(ffmpeg_audio_decoder_tests)

add_executable(ascii_shader_tests)
target_sources(ascii_shader_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_ascii_shader_tests>
  $<TARGET_OBJECTS:radioify_ascii_cpu>
  $<TARGET_OBJECTS:radioify_ascii_gpu>
  $<TARGET_OBJECTS:radioify_image_codec>
)
radioify_link_repo_target(ascii_shader_tests)
if (WIN32)
  target_link_libraries(ascii_shader_tests PRIVATE d3d10 d3d11 dxgi ole32 windowscodecs)
endif()

add_executable(shortcut_match_tests)
target_sources(shortcut_match_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_shortcut_match_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/audio/clock_reacquire.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/control/serial.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/frame_cursor.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/frame_step_source_cache.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/frame_step_seek.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/timing/main_clock.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/state/machine.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/timing/sync.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/enhancement/pipeline.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/enhancement/nvidia_d3d11.cpp
)
radioify_link_repo_target(shortcut_match_tests)
if (WIN32)
  target_link_libraries(shortcut_match_tests PRIVATE d3d11)
endif()
add_test(NAME shortcut_match_tests
         COMMAND $<TARGET_FILE:shortcut_match_tests>)

add_executable(terminal_input_sequence_tests)
target_sources(terminal_input_sequence_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_terminal_input_sequence_tests>
  ${CMAKE_SOURCE_DIR}/tui/terminal_input_sequence.cpp
)
radioify_link_repo_target(terminal_input_sequence_tests)
add_test(NAME terminal_input_sequence_tests
         COMMAND $<TARGET_FILE:terminal_input_sequence_tests>)

add_executable(frame_clipboard_tests)
target_sources(frame_clipboard_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_frame_clipboard_tests>
  $<TARGET_OBJECTS:radioify_image_codec>
  ${CMAKE_SOURCE_DIR}/playback/video/framebuffer/frame_clipboard.cpp
)
radioify_link_repo_target(frame_clipboard_tests)
if (WIN32)
  target_link_libraries(frame_clipboard_tests PRIVATE ole32 windowscodecs user32)
endif()
add_test(NAME frame_clipboard_tests
         COMMAND $<TARGET_FILE:frame_clipboard_tests>)

add_executable(osd_timeline_tests)
target_sources(osd_timeline_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_osd_timeline_tests>
  ${CMAKE_SOURCE_DIR}/playback/session/osd_timeline.cpp
)
radioify_link_repo_target(osd_timeline_tests)
add_test(NAME osd_timeline_tests
         COMMAND $<TARGET_FILE:osd_timeline_tests>)

add_executable(presentation_policy_tests)
target_sources(presentation_policy_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_presentation_policy_tests>
)
radioify_link_repo_target(presentation_policy_tests)
add_test(NAME presentation_policy_tests
         COMMAND $<TARGET_FILE:presentation_policy_tests>)

add_executable(video_output_color_tests)
target_sources(video_output_color_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_video_output_color_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/framebuffer/video_output_color.cpp
)
radioify_link_repo_target(video_output_color_tests)
if (WIN32)
  target_link_libraries(video_output_color_tests PRIVATE dxgi user32)
endif()

add_executable(video_frame_step_smoke)
target_sources(video_frame_step_smoke PRIVATE
  $<TARGET_OBJECTS:radioify_tool_video_frame_step_smoke>
  $<TARGET_OBJECTS:radioify_video_playback>
  $<TARGET_OBJECTS:radioify_ascii_gpu>
  $<TARGET_OBJECTS:radioify_core_runtime>
  $<TARGET_OBJECTS:radioify_audio>
  $<TARGET_OBJECTS:radioify_analysis>
  $<TARGET_OBJECTS:radioify_audiofilter>
  $<TARGET_OBJECTS:radioify_config>
)
radioify_configure_repo_target(video_frame_step_smoke)

add_executable(frame_step_snapshot_tests)
target_sources(frame_step_snapshot_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_frame_step_snapshot_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/frame_step_snapshot.cpp
)
radioify_link_repo_target(frame_step_snapshot_tests)
if (WIN32)
  target_link_libraries(frame_step_snapshot_tests PRIVATE d3d11)
endif()
add_test(NAME frame_step_snapshot_tests
         COMMAND $<TARGET_FILE:frame_step_snapshot_tests>)

add_executable(timeline_preview_tests)
target_sources(timeline_preview_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_timeline_preview_tests>
  $<TARGET_OBJECTS:radioify_timeline_preview>
  $<TARGET_OBJECTS:radioify_image_codec>
  ${CMAKE_SOURCE_DIR}/playback/video/decoder.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/sequence.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/timing/timeline.cpp
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
)
radioify_link_repo_target(timeline_preview_tests)
add_test(NAME timeline_preview_tests
         COMMAND $<TARGET_FILE:timeline_preview_tests>)

add_executable(video_edit_timeline_tests)
target_sources(video_edit_timeline_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_video_edit_timeline_tests>
  ${CMAKE_SOURCE_DIR}/playback/overlay/cell_layout.cpp
  ${CMAKE_SOURCE_DIR}/playback/overlay/controls.cpp
  ${CMAKE_SOURCE_DIR}/playback/overlay/context_menu.cpp
  ${CMAKE_SOURCE_DIR}/playback/overlay/interaction.cpp
  ${CMAKE_SOURCE_DIR}/playback/session/context_menu_controller.cpp
  ${CMAKE_SOURCE_DIR}/playback/media_action_catalog.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/composition/render_plan.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/frame_step_prefetch_mapping.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/sequence.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/scene_suggestions.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/timeline.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/overlay_model.cpp
  ${CMAKE_SOURCE_DIR}/core/unicode_display_width.cpp
)
radioify_link_repo_target(video_edit_timeline_tests)
add_test(NAME video_edit_timeline_tests
         COMMAND $<TARGET_FILE:video_edit_timeline_tests>)

add_executable(video_edit_export_smoke)
target_sources(video_edit_export_smoke PRIVATE
  $<TARGET_OBJECTS:radioify_tool_video_edit_export_smoke>
  ${CMAKE_SOURCE_DIR}/playback/video/edit/export.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/export_default.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/export_codec_support.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/export_dolby_vision.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/export_metadata.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/composition/render_plan.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/export_pipeline.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
)
radioify_link_repo_target(video_edit_export_smoke)
add_test(NAME video_edit_export_cadence_audit_tests
         COMMAND $<TARGET_FILE:video_edit_export_smoke> --self-test)

add_executable(exporter_job_tests)
target_sources(exporter_job_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_exporter_job_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/edit/export.cpp
)
radioify_link_repo_target(exporter_job_tests)
add_test(NAME exporter_job_tests
         COMMAND $<TARGET_FILE:exporter_job_tests>)

add_executable(window_display_lifecycle_tests)
target_sources(window_display_lifecycle_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_window_display_lifecycle_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/framebuffer/window/display_lifecycle.cpp
)
radioify_link_repo_target(window_display_lifecycle_tests)
add_test(NAME window_display_lifecycle_tests
         COMMAND $<TARGET_FILE:window_display_lifecycle_tests>)

add_executable(gpu_text_grid_composition_tests)
target_sources(gpu_text_grid_composition_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_gpu_text_grid_composition_tests>
)
radioify_link_repo_target(gpu_text_grid_composition_tests)
if (WIN32)
  target_link_libraries(gpu_text_grid_composition_tests PRIVATE d3d11)
endif()
add_test(NAME gpu_text_grid_composition_tests
         COMMAND $<TARGET_FILE:gpu_text_grid_composition_tests>)

add_executable(waitable_signal_tests)
target_sources(waitable_signal_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_waitable_signal_tests>
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
)
radioify_link_repo_target(waitable_signal_tests)
add_test(NAME waitable_signal_tests
         COMMAND $<TARGET_FILE:waitable_signal_tests>)

add_executable(thread_dispatch_queue_tests)
target_sources(thread_dispatch_queue_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_thread_dispatch_queue_tests>
  ${CMAKE_SOURCE_DIR}/core/thread_dispatch_queue.cpp
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
)
radioify_link_repo_target(thread_dispatch_queue_tests)
add_test(NAME thread_dispatch_queue_tests
         COMMAND $<TARGET_FILE:thread_dispatch_queue_tests>)

add_executable(window_input_queue_tests)
target_sources(window_input_queue_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_window_input_queue_tests>
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/framebuffer/window/input_queue.cpp
)
radioify_link_repo_target(window_input_queue_tests)
add_test(NAME window_input_queue_tests
         COMMAND $<TARGET_FILE:window_input_queue_tests>)

add_executable(open_file_requests_tests)
target_sources(open_file_requests_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_open_file_requests_tests>
  ${CMAKE_SOURCE_DIR}/core/open_file_requests.cpp
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
)
radioify_link_repo_target(open_file_requests_tests)
add_test(NAME open_file_requests_tests
         COMMAND $<TARGET_FILE:open_file_requests_tests>)

add_executable(shell_open_protocol_tests)
target_sources(shell_open_protocol_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_shell_open_protocol_tests>
  ${CMAKE_SOURCE_DIR}/core/shell_open_protocol.cpp
  ${CMAKE_SOURCE_DIR}/core/utf8.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
)
radioify_link_repo_target(shell_open_protocol_tests)
add_test(NAME shell_open_protocol_tests
         COMMAND $<TARGET_FILE:shell_open_protocol_tests>)

add_executable(utf8_tests)
target_sources(utf8_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_utf8_tests>
  ${CMAKE_SOURCE_DIR}/core/utf8.cpp
)
radioify_link_repo_target(utf8_tests)
add_test(NAME utf8_tests COMMAND $<TARGET_FILE:utf8_tests>)

add_executable(subtitle_pixel_compositor_tests)
target_sources(subtitle_pixel_compositor_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_subtitle_pixel_compositor_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/framebuffer/subtitle_pixel_compositor.cpp
)
radioify_link_repo_target(subtitle_pixel_compositor_tests)
add_test(NAME subtitle_pixel_compositor_tests
         COMMAND $<TARGET_FILE:subtitle_pixel_compositor_tests>)

add_executable(subtitle_sidecar_discovery_tests)
target_sources(subtitle_sidecar_discovery_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_subtitle_sidecar_discovery_tests>
  $<TARGET_OBJECTS:radioify_video_sidecar_identity>
  $<TARGET_OBJECTS:radioify_transcript_artifact>
  ${CMAKE_SOURCE_DIR}/playback/video/subtitle/sidecar_discovery.cpp
)
radioify_link_repo_target(subtitle_sidecar_discovery_tests)
add_test(NAME subtitle_sidecar_discovery_tests
         COMMAND $<TARGET_FILE:subtitle_sidecar_discovery_tests>)

add_executable(browser_navigation_tests)
target_sources(browser_navigation_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_browser_navigation_tests>
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
  ${CMAKE_SOURCE_DIR}/tui/browser_grid_index.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_input_state.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_model.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_navigation.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/optionsbrowser_target.cpp
  ${CMAKE_SOURCE_DIR}/core/path_identity.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/playback_target_match.cpp
)
radioify_link_repo_target(browser_navigation_tests)
add_test(NAME browser_navigation_tests
         COMMAND $<TARGET_FILE:browser_navigation_tests>)

add_executable(browser_directory_listing_tests)
target_sources(browser_directory_listing_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_browser_directory_listing_tests>
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_directory_listing.cpp
  ${CMAKE_SOURCE_DIR}/core/media_artwork_sidecar.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/core/path_identity.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
)
radioify_link_repo_target(browser_directory_listing_tests)
add_test(NAME browser_directory_listing_tests
         COMMAND $<TARGET_FILE:browser_directory_listing_tests>)

add_executable(browser_content_view_tests)
target_sources(browser_content_view_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_browser_content_view_tests>
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_content_view.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_model.cpp
  ${CMAKE_SOURCE_DIR}/core/path_identity.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
)
radioify_link_repo_target(browser_content_view_tests)
add_test(NAME browser_content_view_tests
         COMMAND $<TARGET_FILE:browser_content_view_tests>)

add_executable(popup_menu_tests)
target_sources(popup_menu_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_popup_menu_tests>
  ${CMAKE_SOURCE_DIR}/tui/ui/popup_menu.cpp
  ${CMAKE_SOURCE_DIR}/core/unicode_display_width.cpp
)
radioify_link_repo_target(popup_menu_tests)
add_test(NAME popup_menu_tests
         COMMAND $<TARGET_FILE:popup_menu_tests>)

add_executable(command_palette_tests)
target_sources(command_palette_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_command_palette_tests>
  ${CMAKE_SOURCE_DIR}/tui/ui/command_palette.cpp
)
radioify_link_repo_target(command_palette_tests)
add_test(NAME command_palette_tests
         COMMAND $<TARGET_FILE:command_palette_tests>)

add_executable(browser_action_strip_tests)
target_sources(browser_action_strip_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_browser_action_strip_tests>
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_action_strip.cpp
  ${CMAKE_SOURCE_DIR}/playback/overlay/controls.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/scene_suggestions.cpp
  ${CMAKE_SOURCE_DIR}/core/unicode_display_width.cpp
)
radioify_link_repo_target(browser_action_strip_tests)
add_test(NAME browser_action_strip_tests
         COMMAND $<TARGET_FILE:browser_action_strip_tests>)

add_executable(melody_visualization_tests)
target_sources(melody_visualization_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_melody_visualization_tests>
  ${CMAKE_SOURCE_DIR}/tui/ui/melody_visualization.cpp
)
radioify_link_repo_target(melody_visualization_tests)
add_test(NAME melody_visualization_tests
         COMMAND $<TARGET_FILE:melody_visualization_tests>)

add_executable(tui_theme_tests)
target_sources(tui_theme_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_tui_theme_tests>
  ${CMAKE_SOURCE_DIR}/tui/tui_theme.cpp
)
radioify_link_repo_target(tui_theme_tests)
add_test(NAME tui_theme_tests COMMAND $<TARGET_FILE:tui_theme_tests>)

add_executable(playback_queue_tests)
target_sources(playback_queue_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_playback_queue_tests>
  ${CMAKE_SOURCE_DIR}/app/playback_queue.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/core/path_identity.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/browser_playback_source.cpp
)
radioify_link_repo_target(playback_queue_tests)
add_test(NAME playback_queue_tests
         COMMAND $<TARGET_FILE:playback_queue_tests>)

add_executable(image_viewer_sequence_tests)
target_sources(image_viewer_sequence_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_image_viewer_sequence_tests>
  ${CMAKE_SOURCE_DIR}/core/path_identity.cpp
  ${CMAKE_SOURCE_DIR}/tui/image_viewer_sequence.cpp
)
radioify_link_repo_target(image_viewer_sequence_tests)
add_test(NAME image_viewer_sequence_tests
         COMMAND $<TARGET_FILE:image_viewer_sequence_tests>)

add_executable(media_activation_plan_tests)
target_sources(media_activation_plan_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_media_activation_plan_tests>
  ${CMAKE_SOURCE_DIR}/tui/media_activation_plan.cpp
  ${CMAKE_SOURCE_DIR}/tui/image_viewer_sequence.cpp
  ${CMAKE_SOURCE_DIR}/app/playback_route.cpp
  ${CMAKE_SOURCE_DIR}/playback/target_kind.cpp
  ${CMAKE_SOURCE_DIR}/playback/target_resolver.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/core/path_identity.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
)
radioify_link_repo_target(media_activation_plan_tests)
add_test(NAME media_activation_plan_tests
         COMMAND $<TARGET_FILE:media_activation_plan_tests>)

add_executable(indexed_transcript_tests)
target_sources(indexed_transcript_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_indexed_transcript_tests>
  $<TARGET_OBJECTS:radioify_video_sidecar_identity>
  $<TARGET_OBJECTS:radioify_transcript_artifact>
  $<TARGET_OBJECTS:radioify_transcript_document>
  $<TARGET_OBJECTS:radioify_transcript_cue_semantics>
  $<TARGET_OBJECTS:radioify_transcript_device_policy>
  ${CMAKE_SOURCE_DIR}/playback/video/transcript/subtitle_cues.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/transcript/whisper_model_config.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/core/utf8.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
)
radioify_link_repo_target(indexed_transcript_tests)
add_test(NAME indexed_transcript_tests
         COMMAND $<TARGET_FILE:indexed_transcript_tests>)

add_executable(ui_footer_layout_tests)
target_sources(ui_footer_layout_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_ui_footer_layout_tests>
  ${CMAKE_SOURCE_DIR}/tui/ui/ui_footer_layout.cpp
)
radioify_link_repo_target(ui_footer_layout_tests)
add_test(NAME ui_footer_layout_tests
         COMMAND $<TARGET_FILE:ui_footer_layout_tests>)

add_executable(media_action_catalog_tests)
target_sources(media_action_catalog_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_media_action_catalog_tests>
  ${CMAKE_SOURCE_DIR}/playback/media_action_catalog.cpp
  ${CMAKE_SOURCE_DIR}/playback/media_processing_actions.cpp
)
radioify_link_repo_target(media_action_catalog_tests)
add_test(NAME media_action_catalog_tests
         COMMAND $<TARGET_FILE:media_action_catalog_tests>)

add_executable(subtitle_generation_job_tests)
target_sources(subtitle_generation_job_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_subtitle_generation_job_tests>
  $<TARGET_OBJECTS:radioify_video_sidecar_identity>
  $<TARGET_OBJECTS:radioify_transcript_artifact>
  ${CMAKE_SOURCE_DIR}/playback/video/transcript/generation_job.cpp
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/core/utf8.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
)
radioify_link_repo_target(subtitle_generation_job_tests)
add_test(NAME subtitle_generation_job_tests
         COMMAND $<TARGET_FILE:subtitle_generation_job_tests>)

add_executable(media_processing_coordinator_tests)
target_sources(media_processing_coordinator_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_media_processing_coordinator_tests>
  $<TARGET_OBJECTS:radioify_video_sidecar_identity>
  $<TARGET_OBJECTS:radioify_transcript_artifact>
  ${CMAKE_SOURCE_DIR}/app/media_processing_coordinator.cpp
  ${CMAKE_SOURCE_DIR}/app/media_processing_playback_service.cpp
  ${CMAKE_SOURCE_DIR}/playback/media_processing_actions.cpp
  ${CMAKE_SOURCE_DIR}/tui/ui/media_task_presentation.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/transcript/generation_job.cpp
  ${CMAKE_SOURCE_DIR}/audio/separation/artifact.cpp
  ${CMAKE_SOURCE_DIR}/audio/separation/job.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/core/path_identity.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/core/utf8.cpp
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
)
radioify_link_repo_target(media_processing_coordinator_tests)
add_test(NAME media_processing_coordinator_tests
         COMMAND $<TARGET_FILE:media_processing_coordinator_tests>)

add_executable(whisper_gpu_contract_tests)
target_sources(whisper_gpu_contract_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_whisper_gpu_contract_tests>
)
radioify_link_repo_target(whisper_gpu_contract_tests)
add_test(NAME whisper_gpu_contract_tests
         COMMAND $<TARGET_FILE:whisper_gpu_contract_tests>
                 "${RADIOIFY_WHISPER_MODEL_PATH}")

add_executable(indexed_transcript_smoke)
target_sources(indexed_transcript_smoke PRIVATE
  $<TARGET_OBJECTS:radioify_tool_indexed_transcript_smoke>
  $<TARGET_OBJECTS:radioify_video_sidecar_identity>
  $<TARGET_OBJECTS:radioify_transcript_artifact>
  $<TARGET_OBJECTS:radioify_transcript_document>
  $<TARGET_OBJECTS:radioify_transcript_cue_semantics>
  $<TARGET_OBJECTS:radioify_transcript_device_policy>
  $<TARGET_OBJECTS:radioify_transcript_job>
  $<TARGET_OBJECTS:radioify_transcript_engine>
  ${CMAKE_SOURCE_DIR}/audio/ffmpegaudio.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/core/utf8.cpp
  ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
)
radioify_link_repo_target(indexed_transcript_smoke)

add_executable(scene_analysis_tests)
target_sources(scene_analysis_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_scene_analysis_tests>
  $<TARGET_OBJECTS:radioify_transcript_cue_semantics>
  ${CMAKE_SOURCE_DIR}/playback/video/analysis/scene_analysis.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/scene_suggestions.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/timeline.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/sequence.cpp
)
radioify_link_repo_target(scene_analysis_tests)
add_test(NAME scene_analysis_tests
         COMMAND $<TARGET_FILE:scene_analysis_tests>)

add_executable(scene_analysis_job_tests)
target_sources(scene_analysis_job_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_scene_analysis_job_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/analysis/scene_analysis_job.cpp
)
radioify_link_repo_target(scene_analysis_job_tests)
add_test(NAME scene_analysis_job_tests
         COMMAND $<TARGET_FILE:scene_analysis_job_tests>)

add_executable(scene_suggestion_review_tests)
target_sources(scene_suggestion_review_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_scene_suggestion_review_tests>
  ${CMAKE_SOURCE_DIR}/playback/video/edit/scene_suggestion_review.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/scene_suggestions.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/edit/timeline.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/sequence.cpp
)
radioify_link_repo_target(scene_suggestion_review_tests)
add_test(NAME scene_suggestion_review_tests
         COMMAND $<TARGET_FILE:scene_suggestion_review_tests>)

add_executable(scene_analysis_smoke)
target_sources(scene_analysis_smoke PRIVATE
  $<TARGET_OBJECTS:radioify_tool_scene_analysis_smoke>
  $<TARGET_OBJECTS:radioify_scene_analysis>
  $<TARGET_OBJECTS:radioify_video_sidecar_identity>
  $<TARGET_OBJECTS:radioify_transcript_artifact>
  $<TARGET_OBJECTS:radioify_transcript_document>
  $<TARGET_OBJECTS:radioify_transcript_cue_semantics>
  ${CMAKE_SOURCE_DIR}/playback/video/decoder.cpp
  ${CMAKE_SOURCE_DIR}/playback/video/timing/timeline.cpp
  ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
  ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
)
radioify_link_repo_target(scene_analysis_smoke)

if (RADIOIFY_HAS_AUDIO_SEPARATION)
  add_executable(audio_separation_tests)
  target_sources(audio_separation_tests PRIVATE
    $<TARGET_OBJECTS:radioify_tool_audio_separation_tests>
    $<TARGET_OBJECTS:radioify_audio_separation>
    ${CMAKE_SOURCE_DIR}/audio/ffmpegaudio.cpp
    ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
    ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
    ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
  )
  set_target_properties(audio_separation_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
  radioify_link_repo_target(audio_separation_tests)
  target_link_libraries(audio_separation_tests PRIVATE
    WindowsML::OnnxRuntime)
  radioify_deploy_windows_ml(audio_separation_tests)
  add_test(NAME audio_separation_tests
           COMMAND $<TARGET_FILE:audio_separation_tests>)

  add_executable(audio_separation_smoke)
  target_sources(audio_separation_smoke PRIVATE
    $<TARGET_OBJECTS:radioify_tool_audio_separation_smoke>
    $<TARGET_OBJECTS:radioify_audio_separation>
    ${CMAKE_SOURCE_DIR}/audio/ffmpegaudio.cpp
    ${CMAKE_SOURCE_DIR}/audio/media_formats.cpp
    ${CMAKE_SOURCE_DIR}/core/runtime_helpers.cpp
    ${CMAKE_SOURCE_DIR}/core/waitable_signal.cpp
  )
  radioify_link_repo_target(audio_separation_smoke)
  target_link_libraries(audio_separation_smoke PRIVATE
    WindowsML::OnnxRuntime)
  radioify_deploy_windows_ml(audio_separation_smoke)
endif()

add_executable(windows_console_window_tests)
target_sources(windows_console_window_tests PRIVATE
  $<TARGET_OBJECTS:radioify_tool_windows_console_window_tests>
  ${CMAKE_SOURCE_DIR}/core/windows_console_window.cpp
)
radioify_link_repo_target(windows_console_window_tests)
add_test(NAME windows_console_window_tests
         COMMAND $<TARGET_FILE:windows_console_window_tests>)

if (RADIOIFY_ENABLE_MELODY_ANALYSIS_EFFECTIVE AND RADIOIFY_DOWNLOAD_NEURAL_PITCH_MODEL)
  add_custom_target(radioify_neural_pitch_model ALL
    COMMAND ${CMAKE_COMMAND}
            -DOUTPUT_PATH=${RADIOIFY_NEURAL_PITCH_MODEL_PATH}
            -DURL=${RADIOIFY_NEURAL_PITCH_MODEL_URL}
            -DSHA256=${RADIOIFY_NEURAL_PITCH_MODEL_SHA256}
            -P "${CMAKE_SOURCE_DIR}/cmake/EnsureModelFile.cmake"
    DEPENDS "${CMAKE_SOURCE_DIR}/cmake/EnsureModelFile.cmake"
    COMMENT "Ensuring neural pitch model exists at ${RADIOIFY_NEURAL_PITCH_MODEL_PATH}"
    VERBATIM
  )
  add_dependencies(radioify radioify_neural_pitch_model)
endif()

if (RADIOIFY_DOWNLOAD_WHISPER_MODEL)
  add_custom_target(radioify_whisper_model ALL
    COMMAND ${CMAKE_COMMAND}
            -DOUTPUT_PATH=${RADIOIFY_WHISPER_MODEL_PATH}
            -DURL=${RADIOIFY_WHISPER_MODEL_URL}
            -DSHA256=${RADIOIFY_WHISPER_MODEL_SHA256}
            -P "${CMAKE_SOURCE_DIR}/cmake/EnsureModelFile.cmake"
    DEPENDS "${CMAKE_SOURCE_DIR}/cmake/EnsureModelFile.cmake"
    COMMENT "Ensuring Whisper transcript model exists at ${RADIOIFY_WHISPER_MODEL_PATH}"
    VERBATIM
  )
  add_dependencies(radioify radioify_whisper_model)
endif()

add_custom_target(radioify_whisper_runtime_model ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory
          "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/models"
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${RADIOIFY_WHISPER_MODEL_PATH}"
          "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/models/ggml-base-q5_1.bin"
  COMMENT "Publishing Whisper transcript model beside the Radioify build"
  VERBATIM
)
if (TARGET radioify_whisper_model)
  add_dependencies(radioify_whisper_runtime_model radioify_whisper_model)
endif()
add_dependencies(radioify radioify_whisper_runtime_model)
add_dependencies(whisper_gpu_contract_tests radioify_whisper_runtime_model)
add_dependencies(indexed_transcript_smoke radioify_whisper_runtime_model)

if (RADIOIFY_HAS_AUDIO_SEPARATION)
  add_custom_target(radioify_audio_separation_runtime_model ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/models/audio_separation"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RADIOIFY_AUDIO_SEPARATION_MODEL_PATH}"
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/models/audio_separation/bandit-v2-multi-mask-core-fp16.onnx"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/models/audio_separation/BandIt-v2-MODEL-CARD"
            "${CMAKE_BINARY_DIR}/bin/$<CONFIG>/models/audio_separation/BandIt-v2-MODEL-CARD"
    COMMENT "Publishing BandIt v2 model beside the Radioify build"
    VERBATIM
  )
  add_dependencies(radioify radioify_audio_separation_runtime_model)
  add_dependencies(audio_separation_smoke
                   radioify_audio_separation_runtime_model)
endif()

set(RADIOIFY_SHADER_DIR "${CMAKE_SOURCE_DIR}/playback/render/shaders")
set(RADIOIFY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(RADIOIFY_EMBED_BINARY_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake")

set(RADIOIFY_COMPUTE_SHADER_HLSL "${RADIOIFY_SHADER_DIR}/asciiart_gpu.hlsl")
set(RADIOIFY_ASCII_CONSTANTS_INC
    "${CMAKE_SOURCE_DIR}/playback/video/ascii/asciiart_constants.inc")
set(RADIOIFY_STATS_SHADER_HLSL "${RADIOIFY_SHADER_DIR}/asciiart_stats.hlsl")
set(RADIOIFY_VIDEO_LUMINANCE_STATS_HLSLI
    "${RADIOIFY_SHADER_DIR}/video_luminance_stats.hlsli")
set(RADIOIFY_VIDEO_COLOR_MATH_HLSLI
    "${RADIOIFY_SHADER_DIR}/video_color_math.hlsli")
set(RADIOIFY_VIDEO_HDR_TO_SDR_HLSLI
    "${RADIOIFY_SHADER_DIR}/video_hdr_to_sdr.hlsli")
set(RADIOIFY_WINDOW_RENDER_HLSL "${RADIOIFY_SHADER_DIR}/window/render.hlsl")
set(RADIOIFY_WINDOW_OVERLAY_HLSL "${RADIOIFY_SHADER_DIR}/window/overlay.hlsl")
set(RADIOIFY_WINDOW_GPU_TEXT_GRID_HLSL "${RADIOIFY_SHADER_DIR}/window/gpu_text_grid.hlsl")
set(RADIOIFY_WINDOW_HDR_GENERATE_HLSLI "${RADIOIFY_SHADER_DIR}/window/hdr_generate.hlsli")
set(RADIOIFY_ASCII_GPU_SHADER_DEPS
  "${RADIOIFY_COMPUTE_SHADER_HLSL}"
  "${RADIOIFY_ASCII_CONSTANTS_INC}"
  "${RADIOIFY_VIDEO_HDR_TO_SDR_HLSLI}"
  "${RADIOIFY_VIDEO_COLOR_MATH_HLSLI}"
)
set(RADIOIFY_ASCII_STATS_SHADER_DEPS
  "${RADIOIFY_STATS_SHADER_HLSL}"
  "${RADIOIFY_ASCII_CONSTANTS_INC}"
  "${RADIOIFY_VIDEO_LUMINANCE_STATS_HLSLI}"
  "${RADIOIFY_VIDEO_HDR_TO_SDR_HLSLI}"
  "${RADIOIFY_VIDEO_COLOR_MATH_HLSLI}"
)
set(RADIOIFY_WINDOW_HDR_SHADER_DEPS
  "${RADIOIFY_WINDOW_HDR_GENERATE_HLSLI}"
  "${RADIOIFY_VIDEO_COLOR_MATH_HLSLI}"
)
set(RADIOIFY_COMPUTE_SHADER_CSO_MAIN
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main_nv12.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_dot_prefilter.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_dot_prefilter_nv12.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_DOT_EDGE
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_dot_edge.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_REFINE_CONTINUITY
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_refine_continuity.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_sync_history.cso")
set(RADIOIFY_STATS_SHADER_CSO_RGBA
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_rgba.cso")
set(RADIOIFY_STATS_SHADER_CSO_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_nv12.cso")
set(RADIOIFY_WINDOW_RENDER_CSO_VS
    "${RADIOIFY_GENERATED_DIR}/window_render_vs.cso")
set(RADIOIFY_WINDOW_RENDER_CSO_PS
    "${RADIOIFY_GENERATED_DIR}/window_render_ps.cso")
set(RADIOIFY_WINDOW_OVERLAY_CSO_PS
    "${RADIOIFY_GENERATED_DIR}/window_overlay_ps.cso")
set(RADIOIFY_WINDOW_GPU_TEXT_GRID_CSO_PS
    "${RADIOIFY_GENERATED_DIR}/window_gpu_text_grid_ps.cso")

set(RADIOIFY_COMPUTE_SHADER_HEADER_MAIN
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_MAIN_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main_nv12.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_dot_prefilter.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_dot_prefilter_nv12.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_DOT_EDGE
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_dot_edge.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_REFINE_CONTINUITY
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_refine_continuity.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_SYNC_HISTORY
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_sync_history.h")
set(RADIOIFY_STATS_SHADER_HEADER_RGBA
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_rgba.h")
set(RADIOIFY_STATS_SHADER_HEADER_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_nv12.h")
set(RADIOIFY_WINDOW_RENDER_HEADER_VS
    "${RADIOIFY_GENERATED_DIR}/window_render_vs.h")
set(RADIOIFY_WINDOW_RENDER_HEADER_PS
    "${RADIOIFY_GENERATED_DIR}/window_render_ps.h")
set(RADIOIFY_WINDOW_OVERLAY_HEADER_PS
    "${RADIOIFY_GENERATED_DIR}/window_overlay_ps.h")
set(RADIOIFY_WINDOW_GPU_TEXT_GRID_HEADER_PS
    "${RADIOIFY_GENERATED_DIR}/window_gpu_text_grid_ps.h")

if (WIN32)
  set(_radioify_fxc_hints "")
  if (DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    list(APPEND _radioify_fxc_hints
      "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}"
      "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64"
      "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x86")
  endif()
  if (CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
    list(APPEND _radioify_fxc_hints
      "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}"
      "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
      "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x86")
  endif()

  if (NOT RADIOIFY_FXC_EXE)
    find_program(RADIOIFY_FXC_EXE NAMES fxc HINTS ${_radioify_fxc_hints})
  endif()
  if (NOT RADIOIFY_FXC_EXE)
    message(FATAL_ERROR "fxc not found. Install the Windows SDK or set RADIOIFY_FXC_EXE.")
  endif()

  set(_radioify_fxc_flags /nologo /T cs_5_0 /O3 /Qstrip_debug /Qstrip_reflect)
  set(_radioify_fxc_flags_vs /nologo /T vs_5_0 /O3 /Qstrip_debug /Qstrip_reflect)
  set(_radioify_fxc_flags_ps /nologo /T ps_5_0 /O3 /Qstrip_debug /Qstrip_reflect)

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSMain
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_GPU_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSMain
            /D NV12_INPUT=1
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_GPU_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSDotPrefilter
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_GPU_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER_NV12}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSDotPrefilter
            /D NV12_INPUT=1
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER_NV12}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_GPU_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_EDGE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSDotEdge
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_EDGE}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_GPU_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_STATS_SHADER_CSO_RGBA}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSStats
            /D RGBA_INPUT=1
            /Fo "${RADIOIFY_STATS_SHADER_CSO_RGBA}"
            "${RADIOIFY_STATS_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_STATS_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_STATS_SHADER_CSO_NV12}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSStats
            /D NV12_INPUT=1
            /Fo "${RADIOIFY_STATS_SHADER_CSO_NV12}"
            "${RADIOIFY_STATS_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_STATS_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSSyncHistory
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_GPU_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_REFINE_CONTINUITY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSRefineContinuity
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_REFINE_CONTINUITY}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS ${RADIOIFY_ASCII_GPU_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_MAIN}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN}
            -DVAR=kComputeShaderCsMain
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN_NV12}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN_NV12}
            -DVAR=kComputeShaderCsMainNv12
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER}
            -DVAR=kComputeShaderCsDotPrefilter
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER_NV12}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER_NV12}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER_NV12}
            -DVAR=kComputeShaderCsDotPrefilterNv12
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_PREFILTER_NV12}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_EDGE}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_DOT_EDGE}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_EDGE}
            -DVAR=kComputeShaderCsDotEdge
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_DOT_EDGE}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_RENDER_CSO_VS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags_vs} /E VS
            /Fo "${RADIOIFY_WINDOW_RENDER_CSO_VS}"
            "${RADIOIFY_WINDOW_RENDER_HLSL}"
    DEPENDS "${RADIOIFY_WINDOW_RENDER_HLSL}"
            ${RADIOIFY_WINDOW_HDR_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_RENDER_CSO_PS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags_ps} /E PS
            /Fo "${RADIOIFY_WINDOW_RENDER_CSO_PS}"
            "${RADIOIFY_WINDOW_RENDER_HLSL}"
    DEPENDS "${RADIOIFY_WINDOW_RENDER_HLSL}"
            ${RADIOIFY_WINDOW_HDR_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_OVERLAY_CSO_PS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags_ps} /E PS_UI
            /Fo "${RADIOIFY_WINDOW_OVERLAY_CSO_PS}"
            "${RADIOIFY_WINDOW_OVERLAY_HLSL}"
    DEPENDS "${RADIOIFY_WINDOW_OVERLAY_HLSL}"
            ${RADIOIFY_WINDOW_HDR_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_GPU_TEXT_GRID_CSO_PS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags_ps} /E PS_GPU_TEXT_GRID
            /Fo "${RADIOIFY_WINDOW_GPU_TEXT_GRID_CSO_PS}"
            "${RADIOIFY_WINDOW_GPU_TEXT_GRID_HLSL}"
    DEPENDS "${RADIOIFY_WINDOW_GPU_TEXT_GRID_HLSL}"
            ${RADIOIFY_WINDOW_HDR_SHADER_DEPS}
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_SYNC_HISTORY}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_SYNC_HISTORY}
            -DVAR=kComputeShaderCsSyncHistory
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_REFINE_CONTINUITY}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_REFINE_CONTINUITY}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_REFINE_CONTINUITY}
            -DVAR=kComputeShaderCsRefineContinuity
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_REFINE_CONTINUITY}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_STATS_SHADER_HEADER_RGBA}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_STATS_SHADER_CSO_RGBA}
            -DOUTPUT=${RADIOIFY_STATS_SHADER_HEADER_RGBA}
            -DVAR=kStatsShaderCsRgba
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_STATS_SHADER_CSO_RGBA}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_STATS_SHADER_HEADER_NV12}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_STATS_SHADER_CSO_NV12}
            -DOUTPUT=${RADIOIFY_STATS_SHADER_HEADER_NV12}
            -DVAR=kStatsShaderCsNv12
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_STATS_SHADER_CSO_NV12}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_RENDER_HEADER_VS}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_WINDOW_RENDER_CSO_VS}
            -DOUTPUT=${RADIOIFY_WINDOW_RENDER_HEADER_VS}
            -DVAR=kWindowVs
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_WINDOW_RENDER_CSO_VS}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_RENDER_HEADER_PS}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_WINDOW_RENDER_CSO_PS}
            -DOUTPUT=${RADIOIFY_WINDOW_RENDER_HEADER_PS}
            -DVAR=kWindowPs
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_WINDOW_RENDER_CSO_PS}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_OVERLAY_HEADER_PS}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_WINDOW_OVERLAY_CSO_PS}
            -DOUTPUT=${RADIOIFY_WINDOW_OVERLAY_HEADER_PS}
            -DVAR=kWindowPsUi
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_WINDOW_OVERLAY_CSO_PS}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_WINDOW_GPU_TEXT_GRID_HEADER_PS}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_WINDOW_GPU_TEXT_GRID_CSO_PS}
            -DOUTPUT=${RADIOIFY_WINDOW_GPU_TEXT_GRID_HEADER_PS}
            -DVAR=kWindowPsGpuTextGrid
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_WINDOW_GPU_TEXT_GRID_CSO_PS}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  set(RADIOIFY_SHADER_HEADERS
    "${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN_NV12}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_PREFILTER_NV12}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_DOT_EDGE}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_REFINE_CONTINUITY}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_SYNC_HISTORY}"
    "${RADIOIFY_STATS_SHADER_HEADER_RGBA}"
    "${RADIOIFY_STATS_SHADER_HEADER_NV12}"
    "${RADIOIFY_WINDOW_RENDER_HEADER_VS}"
    "${RADIOIFY_WINDOW_RENDER_HEADER_PS}"
    "${RADIOIFY_WINDOW_OVERLAY_HEADER_PS}"
    "${RADIOIFY_WINDOW_GPU_TEXT_GRID_HEADER_PS}"
  )

  add_custom_target(radioify_shader_blobs
    DEPENDS ${RADIOIFY_SHADER_HEADERS}
  )

  add_dependencies(radioify radioify_shader_blobs)
  add_dependencies(radioify_video radioify_shader_blobs)
  add_dependencies(radioify_ascii_gpu radioify_shader_blobs)
  target_include_directories(radioify_project_options INTERFACE "${RADIOIFY_GENERATED_DIR}")
  target_sources(radioify PRIVATE ${RADIOIFY_SHADER_HEADERS})
endif()

set(RADIOIFY_BUILD_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${RADIOIFY_BUILD_RUNTIME_OUTPUT_DIRECTORY}")
foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper}
      "${RADIOIFY_BUILD_RUNTIME_OUTPUT_DIRECTORY}/${config}")
endforeach()

# vcpkg's FFmpeg port ships a `FindFFMPEG.cmake` module under
# `vcpkg_installed/<triplet>/share/ffmpeg/`. If the vcpkg toolchain file wasn't
# provided to CMake (common when running CMake directly), `find_package(FFMPEG)`
# won't see that module. Make manifest-mode builds work by adding the module path
# when `VCPKG_INSTALLED_DIR` (or a local `vcpkg_installed/`) is present.
if (WIN32)
  set(_radioify_vcpkg_installed_dir "")
  if (DEFINED VCPKG_INSTALLED_DIR)
    set(_radioify_vcpkg_installed_dir "${VCPKG_INSTALLED_DIR}")
  elseif (DEFINED ENV{VCPKG_INSTALLED_DIR})
    set(_radioify_vcpkg_installed_dir "$ENV{VCPKG_INSTALLED_DIR}")
  elseif (EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed")
    set(_radioify_vcpkg_installed_dir "${CMAKE_SOURCE_DIR}/vcpkg_installed")
  endif()

  if (_radioify_vcpkg_installed_dir)
    set(_radioify_vcpkg_triplet "")
    if (DEFINED VCPKG_TARGET_TRIPLET)
      set(_radioify_vcpkg_triplet "${VCPKG_TARGET_TRIPLET}")
    elseif (DEFINED ENV{VCPKG_TARGET_TRIPLET})
      set(_radioify_vcpkg_triplet "$ENV{VCPKG_TARGET_TRIPLET}")
    endif()

    if (NOT _radioify_vcpkg_triplet)
      set(_radioify_vcpkg_triplets "")
      file(GLOB _radioify_vcpkg_triplet_dirs LIST_DIRECTORIES true "${_radioify_vcpkg_installed_dir}/*")
      foreach (_dir IN LISTS _radioify_vcpkg_triplet_dirs)
        if (IS_DIRECTORY "${_dir}")
          get_filename_component(_name "${_dir}" NAME)
          if (NOT _name STREQUAL "vcpkg")
            list(APPEND _radioify_vcpkg_triplets "${_name}")
          endif()
        endif()
      endforeach()

      list(LENGTH _radioify_vcpkg_triplets _radioify_vcpkg_triplet_count)
      if (_radioify_vcpkg_triplet_count EQUAL 1)
        list(GET _radioify_vcpkg_triplets 0 _radioify_vcpkg_triplet)
      endif()
    endif()

    if (NOT _radioify_vcpkg_triplet)
      if (CMAKE_GENERATOR_PLATFORM STREQUAL "Win32")
        set(_radioify_vcpkg_triplet "x86-windows")
      elseif (CMAKE_GENERATOR_PLATFORM STREQUAL "x64")
        set(_radioify_vcpkg_triplet "x64-windows")
      elseif (CMAKE_GENERATOR_PLATFORM STREQUAL "ARM64")
        set(_radioify_vcpkg_triplet "arm64-windows")
      elseif (CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_radioify_vcpkg_triplet "x64-windows")
      else()
        set(_radioify_vcpkg_triplet "x86-windows")
      endif()
    endif()

    set(_radioify_vcpkg_triplet_root "${_radioify_vcpkg_installed_dir}/${_radioify_vcpkg_triplet}")
    if (EXISTS "${_radioify_vcpkg_triplet_root}")
      if (NOT "${_radioify_vcpkg_triplet_root}" IN_LIST CMAKE_PREFIX_PATH)
        list(PREPEND CMAKE_PREFIX_PATH "${_radioify_vcpkg_triplet_root}")
      endif()

      if (EXISTS "${_radioify_vcpkg_triplet_root}/share/ffmpeg/FindFFMPEG.cmake")
        set(_radioify_ffmpeg_module_dir "${_radioify_vcpkg_triplet_root}/share/ffmpeg")
        if (NOT "${_radioify_ffmpeg_module_dir}" IN_LIST CMAKE_MODULE_PATH)
          list(PREPEND CMAKE_MODULE_PATH "${_radioify_ffmpeg_module_dir}")
        endif()
      endif()
    endif()
  endif()
endif()

find_package(FFMPEG QUIET)

if (NOT FFMPEG_FOUND)
  find_package(PkgConfig QUIET)
  if (PkgConfig_FOUND)
    pkg_check_modules(FFMPEG QUIET libavformat libavcodec libavutil libswscale libswresample)
  endif()
endif()

if (NOT FFMPEG_FOUND)
  set(_radioify_ffmpeg_hint_dirs)
  if (DEFINED FFMPEG_DIR AND FFMPEG_DIR)
    list(APPEND _radioify_ffmpeg_hint_dirs "${FFMPEG_DIR}")
  endif()
  if (DEFINED FFMPEG_ROOT AND FFMPEG_ROOT)
    list(APPEND _radioify_ffmpeg_hint_dirs "${FFMPEG_ROOT}")
  endif()
  find_path(FFMPEG_INCLUDE_DIR
    NAMES libavformat/avformat.h
    HINTS ${_radioify_ffmpeg_hint_dirs} ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES include
  )
  find_library(AVFORMAT_LIBRARY NAMES avformat
    HINTS ${_radioify_ffmpeg_hint_dirs} ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(AVCODEC_LIBRARY NAMES avcodec
    HINTS ${_radioify_ffmpeg_hint_dirs} ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(AVUTIL_LIBRARY NAMES avutil
    HINTS ${_radioify_ffmpeg_hint_dirs} ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(SWSCALE_LIBRARY NAMES swscale
    HINTS ${_radioify_ffmpeg_hint_dirs} ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(SWRESAMPLE_LIBRARY NAMES swresample
    HINTS ${_radioify_ffmpeg_hint_dirs} ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  if (FFMPEG_INCLUDE_DIR AND AVFORMAT_LIBRARY AND AVCODEC_LIBRARY AND
      AVUTIL_LIBRARY AND SWSCALE_LIBRARY AND SWRESAMPLE_LIBRARY)
    set(FFMPEG_FOUND TRUE)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
    set(FFMPEG_LIBRARIES
      "${AVFORMAT_LIBRARY}"
      "${AVCODEC_LIBRARY}"
      "${AVUTIL_LIBRARY}"
      "${SWSCALE_LIBRARY}"
      "${SWRESAMPLE_LIBRARY}"
    )
  endif()
endif()

if (NOT FFMPEG_FOUND)
  message(FATAL_ERROR "FFmpeg not found. Set FFMPEG_DIR or install dev libraries.")
endif()

radioify_add_external_include_dirs(${FFMPEG_INCLUDE_DIRS})
if (FFMPEG_LIBRARY_DIRS)
  target_link_directories(radioify PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(ffmpeg_audio_decoder_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(shortcut_match_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(video_frame_step_smoke PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(timeline_preview_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(video_edit_export_smoke PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(video_edit_timeline_tests PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(indexed_transcript_smoke PRIVATE ${FFMPEG_LIBRARY_DIRS})
  target_link_directories(scene_analysis_smoke PRIVATE ${FFMPEG_LIBRARY_DIRS})
  if (RADIOIFY_HAS_AUDIO_SEPARATION)
    target_link_directories(audio_separation_tests PRIVATE
                            ${FFMPEG_LIBRARY_DIRS})
    target_link_directories(audio_separation_smoke PRIVATE
                            ${FFMPEG_LIBRARY_DIRS})
  endif()
endif()

target_link_libraries(radioify PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(ffmpeg_audio_decoder_tests PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(shortcut_match_tests PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(video_frame_step_smoke PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(timeline_preview_tests PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(video_edit_export_smoke PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(video_edit_timeline_tests PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(indexed_transcript_smoke PRIVATE ${FFMPEG_LIBRARIES})
target_link_libraries(scene_analysis_smoke PRIVATE ${FFMPEG_LIBRARIES})
if (RADIOIFY_HAS_AUDIO_SEPARATION)
  target_link_libraries(audio_separation_tests PRIVATE ${FFMPEG_LIBRARIES})
  target_link_libraries(audio_separation_smoke PRIVATE ${FFMPEG_LIBRARIES})
endif()

set(RADIOIFY_HAS_LIBASS OFF)
set(_radioify_libass_include_dirs "")
set(_radioify_libass_libraries "")
set(_radioify_libass_hints "")

function(radioify_find_library_pair output_var release_var debug_var)
  find_library(${release_var}
    NAMES ${ARGN}
    HINTS ENV LIBASS_DIR ENV LIBASS_ROOT ${_radioify_libass_hints}
    PATH_SUFFIXES lib
  )
  find_library(${debug_var}
    NAMES ${ARGN}
    HINTS ENV LIBASS_DIR ENV LIBASS_ROOT ${_radioify_libass_hints}
    PATH_SUFFIXES debug/lib
  )

  set(_radioify_library_pair "")
  if (${release_var} AND ${debug_var})
    set(_radioify_library_pair
      optimized ${${release_var}}
      debug ${${debug_var}}
    )
  elseif (${release_var})
    set(_radioify_library_pair ${${release_var}})
  elseif (${debug_var})
    set(_radioify_library_pair ${${debug_var}})
  endif()

  set(${output_var} ${_radioify_library_pair} PARENT_SCOPE)
endfunction()

if (DEFINED VCPKG_INSTALLED_DIR)
  if (DEFINED VCPKG_TARGET_TRIPLET)
    list(APPEND _radioify_libass_hints "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
  endif()
  if (EXISTS "${VCPKG_INSTALLED_DIR}/x64-windows/tools/pkgconf/pkgconf.exe")
    set(PKG_CONFIG_EXECUTABLE
      "${VCPKG_INSTALLED_DIR}/x64-windows/tools/pkgconf/pkgconf.exe")
  elseif (EXISTS "${VCPKG_INSTALLED_DIR}/x86-windows/tools/pkgconf/pkgconf.exe")
    set(PKG_CONFIG_EXECUTABLE
      "${VCPKG_INSTALLED_DIR}/x86-windows/tools/pkgconf/pkgconf.exe")
  elseif (EXISTS "${VCPKG_INSTALLED_DIR}/arm64-windows/tools/pkgconf/pkgconf.exe")
    set(PKG_CONFIG_EXECUTABLE
      "${VCPKG_INSTALLED_DIR}/arm64-windows/tools/pkgconf/pkgconf.exe")
  endif()
endif()

find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
  set(_radioify_saved_pkg_config_argn "${PKG_CONFIG_ARGN}")
  if (DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static")
    set(PKG_CONFIG_ARGN --static)
  endif()
  pkg_check_modules(RADIOIFY_LIBASS QUIET IMPORTED_TARGET libass)
  set(PKG_CONFIG_ARGN "${_radioify_saved_pkg_config_argn}")
endif()

if (RADIOIFY_LIBASS_FOUND)
  set(RADIOIFY_HAS_LIBASS ON)
  set(_radioify_libass_include_dirs ${RADIOIFY_LIBASS_INCLUDE_DIRS})
  if (TARGET PkgConfig::RADIOIFY_LIBASS)
    set(_radioify_libass_libraries PkgConfig::RADIOIFY_LIBASS)
  else()
    set(_radioify_libass_libraries ${RADIOIFY_LIBASS_LIBRARIES})
  endif()
elseif (NOT RADIOIFY_HAS_LIBASS)
  find_path(RADIOIFY_LIBASS_INCLUDE_DIR
    NAMES ass/ass.h
    HINTS ENV LIBASS_DIR ENV LIBASS_ROOT ${_radioify_libass_hints}
    PATH_SUFFIXES include
  )
  radioify_find_library_pair(_radioify_libass_library
    RADIOIFY_LIBASS_LIBRARY_RELEASE
    RADIOIFY_LIBASS_LIBRARY_DEBUG
    ass libass
  )
  if (RADIOIFY_LIBASS_INCLUDE_DIR AND _radioify_libass_library)
    set(RADIOIFY_HAS_LIBASS ON)
    set(_radioify_libass_include_dirs ${RADIOIFY_LIBASS_INCLUDE_DIR})
    set(_radioify_libass_libraries ${_radioify_libass_library})

    if (DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static")
      set(_radioify_libass_dep_libraries "")
      foreach(_radioify_ass_dep IN ITEMS
          freetype
          harfbuzz
          fribidi
          brotlidec
          brotlicommon
          bz2
          libpng16
          png16
          zlib)
        string(TOUPPER "${_radioify_ass_dep}" _radioify_ass_dep_upper)
        radioify_find_library_pair(_radioify_ass_dep_libraries
          "RADIOIFY_LIBASS_DEP_${_radioify_ass_dep_upper}_RELEASE"
          "RADIOIFY_LIBASS_DEP_${_radioify_ass_dep_upper}_DEBUG"
          ${_radioify_ass_dep} ${_radioify_ass_dep}d
        )
        if (_radioify_ass_dep_libraries)
          list(APPEND _radioify_libass_dep_libraries
            ${_radioify_ass_dep_libraries})
        endif()
      endforeach()
      list(APPEND _radioify_libass_libraries ${_radioify_libass_dep_libraries})
    endif()
  endif()
endif()

if (NOT RADIOIFY_HAS_LIBASS)
  message(FATAL_ERROR
    "libass is required for ASS subtitle rendering. Install libass (for vcpkg: `vcpkg install libass:x64-windows`) and reconfigure.")
endif()
radioify_add_external_include_dirs(${_radioify_libass_include_dirs})
if (RADIOIFY_LIBASS_LIBRARY_DIRS)
  target_link_directories(radioify PRIVATE ${RADIOIFY_LIBASS_LIBRARY_DIRS})
  target_link_directories(video_frame_step_smoke PRIVATE ${RADIOIFY_LIBASS_LIBRARY_DIRS})
endif()
target_link_libraries(radioify PRIVATE ${_radioify_libass_libraries})
target_link_libraries(video_frame_step_smoke PRIVATE ${_radioify_libass_libraries})
message(STATUS "libass found: ASS advanced renderer enabled")

find_package(whisper CONFIG REQUIRED)
if (NOT TARGET whisper)
  message(FATAL_ERROR "whisper-cpp was found without its whisper CMake target.")
endif()
if (NOT GGML_VULKAN)
  message(FATAL_ERROR
    "whisper-cpp must be built with its Vulkan feature; CPU-only indexed transcripts are disabled.")
endif()
target_link_libraries(radioify_transcript_engine PRIVATE whisper)
target_link_libraries(radioify_tool_whisper_gpu_contract_tests PRIVATE whisper)
target_link_libraries(radioify PRIVATE whisper)
target_link_libraries(whisper_gpu_contract_tests PRIVATE whisper)
target_link_libraries(indexed_transcript_smoke PRIVATE whisper)
message(STATUS
  "whisper.cpp found: offline indexed transcripts enabled (Vulkan GPU)")

target_compile_definitions(radioify_project_options INTERFACE
  RADIOIFY_HAS_LIBASS=$<IF:$<BOOL:${RADIOIFY_HAS_LIBASS}>,1,0>
)

set(RADIOIFY_HAS_ONNXRUNTIME OFF)
if (RADIOIFY_ENABLE_MELODY_ANALYSIS_EFFECTIVE)
  set(_radioify_onnx_target "")
  find_package(onnxruntime CONFIG QUIET)
  if (TARGET onnxruntime::onnxruntime)
    set(_radioify_onnx_target onnxruntime::onnxruntime)
  elseif (TARGET onnxruntime)
    set(_radioify_onnx_target onnxruntime)
  endif()

  if (_radioify_onnx_target)
    set(RADIOIFY_HAS_ONNXRUNTIME ON)
    target_link_libraries(radioify_analysis PRIVATE ${_radioify_onnx_target})
    target_link_libraries(radioify PRIVATE ${_radioify_onnx_target})
    message(STATUS "Neural pitch: ONNX Runtime enabled via target ${_radioify_onnx_target}.")
  else()
    find_path(ONNXRUNTIME_INCLUDE_DIR
      NAMES onnxruntime_c_api.h onnxruntime/core/session/onnxruntime_c_api.h
      HINTS ENV ONNXRUNTIME_DIR ENV ONNXRUNTIME_ROOT
      PATH_SUFFIXES include include/onnxruntime/core/session
    )
    find_library(ONNXRUNTIME_LIBRARY
      NAMES onnxruntime
      HINTS ENV ONNXRUNTIME_DIR ENV ONNXRUNTIME_ROOT
      PATH_SUFFIXES lib
    )
    if (ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
      set(RADIOIFY_HAS_ONNXRUNTIME ON)
      radioify_add_external_include_dirs(${ONNXRUNTIME_INCLUDE_DIR})
      target_link_libraries(radioify PRIVATE ${ONNXRUNTIME_LIBRARY})
      message(STATUS "Neural pitch: ONNX Runtime enabled via manual lookup.")
    else()
      message(FATAL_ERROR "Melody analysis requested but ONNX Runtime not found (tried CMake package and manual lookup).")
    endif()
  endif()
endif()

target_compile_definitions(radioify_project_options INTERFACE
  RADIOIFY_ENABLE_MELODY_ANALYSIS=$<IF:$<BOOL:${RADIOIFY_ENABLE_MELODY_ANALYSIS_EFFECTIVE}>,1,0>
  RADIOIFY_ENABLE_NEURAL_PITCH=$<IF:$<BOOL:${RADIOIFY_ENABLE_MELODY_ANALYSIS_EFFECTIVE}>,1,0>
  RADIOIFY_HAS_ONNXRUNTIME=$<IF:$<BOOL:${RADIOIFY_HAS_ONNXRUNTIME}>,1,0>
)

find_package(ZLIB REQUIRED)
add_library(sexypsf STATIC
  ${CMAKE_SOURCE_DIR}/third_party/psx/PsxBios.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/PsxCounters.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/PsxDma.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/Spu.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/PsxHw.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/PsxMem.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/Misc.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/R3000A.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/PsxInterpreter.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/PsxHLE.c
  ${CMAKE_SOURCE_DIR}/third_party/psx/spu/spu.c
)
radioify_configure_external_target(sexypsf)
target_include_directories(sexypsf PUBLIC ${CMAKE_SOURCE_DIR}/third_party/psx)
if (WIN32)
  target_compile_definitions(sexypsf PRIVATE PSS_STYLE=2)
else()
  target_compile_definitions(sexypsf PRIVATE PSS_STYLE=1)
endif()
target_compile_definitions(sexypsf PRIVATE SPSFVERSION="\"0.4.8\"")
if (MSVC)
  target_compile_definitions(sexypsf PRIVATE strcasecmp=_stricmp strncasecmp=_strnicmp)
  target_compile_options(sexypsf PRIVATE /Zp1)
  if (CMAKE_C_COMPILER_ID STREQUAL "Clang")
    target_compile_options(sexypsf PRIVATE
      $<$<COMPILE_LANGUAGE:C>:/clang:-fcommon>
    )
  endif()
endif()
target_link_libraries(sexypsf PUBLIC ZLIB::ZLIB)
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libs" FORCE)
FetchContent_Declare(
  psf2wav
  GIT_REPOSITORY https://github.com/mitsuman/psf2wav.git
  GIT_TAG e07e01a816ae74ba9c7eeaffcd6d9bf7fb294afc
)
cmake_policy(PUSH)
if (POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(psf2wav)
if (NOT psf2wav_POPULATED)
  FetchContent_Populate(psf2wav)
endif()
cmake_policy(POP)

add_library(psfcore STATIC
  ${psf2wav_SOURCE_DIR}/highly_experimental/psflib/psflib.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/psflib/psf2fs.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/bios.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/iop.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/ioptimer.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/psx.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/r3000.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/spu.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/spucore.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/vfs.c
  ${psf2wav_SOURCE_DIR}/highly_experimental/Highly_Experimental/Core/mkhebios.c
)
radioify_configure_external_target(psfcore)
target_include_directories(psfcore PUBLIC
  ${psf2wav_SOURCE_DIR}/highly_experimental
)
target_compile_definitions(psfcore PRIVATE EMU_COMPILE EMU_LITTLE_ENDIAN)
target_link_libraries(psfcore PUBLIC ZLIB::ZLIB)
target_link_libraries(radioify_audio PRIVATE psfcore)
target_link_libraries(radioify PRIVATE psfcore)
target_link_libraries(radioify PRIVATE sexypsf)
target_link_libraries(video_frame_step_smoke PRIVATE psfcore sexypsf)

set(_radioify_warn_deprecated_defined OFF)
if (DEFINED CMAKE_WARN_DEPRECATED)
  set(_radioify_warn_deprecated_defined ON)
  set(_radioify_warn_deprecated_value ${CMAKE_WARN_DEPRECATED})
endif()
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "Suppress third-party CMake deprecation warnings" FORCE)
set(CMAKE_WARN_DEPRECATED OFF)
# Allow older third-party projects with cmake_minimum_required < 3.5 to be
# configured by setting a minimum policy version. This mirrors passing
# -DCMAKE_POLICY_VERSION_MINIMUM=3.5 on the command line.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "Minimum CMake policy version for third-party projects" FORCE)
FetchContent_Declare(
  libkss
  GIT_REPOSITORY https://github.com/digital-sound-antiques/libkss.git
  GIT_TAG 1.2.1
  GIT_SUBMODULES_RECURSE TRUE
)
# Populate libkss so we can apply a tiny compatibility patch before configuring it.
# Newer CMake releases remove compatibility for <3.5; libkss declares 3.0 which
# causes configuration to fail. We populate, patch its CMakeLists to require 3.5,
# then add it via add_subdirectory so the patched file is used.
FetchContent_Populate(libkss)
# Replace any cmake_minimum_required(VERSION x.y) in the downloaded libkss CMakeLists
# files with a minimum of 3.5 so newer CMake can configure it.
if(EXISTS "${libkss_SOURCE_DIR}")
  file(GLOB_RECURSE _radioify_libkss_cmake_files "${libkss_SOURCE_DIR}/**/CMakeLists.txt")
  foreach(_radioify_libkss_file IN LISTS _radioify_libkss_cmake_files)
    file(READ "${_radioify_libkss_file}" _radioify_libkss_contents)
    string(REGEX REPLACE "cmake_minimum_required\\(VERSION[ \\t]*[0-9]+\\.[0-9]+\\)" "cmake_minimum_required(VERSION 3.5)" _radioify_libkss_patched "${_radioify_libkss_contents}")
    if(NOT _radioify_libkss_patched STREQUAL _radioify_libkss_contents)
      file(WRITE "${_radioify_libkss_file}" "${_radioify_libkss_patched}")
    endif()
  endforeach()
endif()

add_subdirectory(${libkss_SOURCE_DIR} ${libkss_BINARY_DIR})
set(_radioify_libkss_kssplay_c "${libkss_SOURCE_DIR}/src/kssplay.c")
if (EXISTS "${_radioify_libkss_kssplay_c}")
  file(READ "${_radioify_libkss_kssplay_c}" _radioify_libkss_kssplay_c_contents)
  string(REPLACE
    "void KSSPLAY_set_channel_mask(KSSPLAY *kssplay, uint32_t device, uint32_t mask) {"
    "void KSSPLAY_set_channel_mask(KSSPLAY *kssplay, KSS_DEVICE device, uint32_t mask) {"
    _radioify_libkss_kssplay_c_patched
    "${_radioify_libkss_kssplay_c_contents}")
  if (NOT _radioify_libkss_kssplay_c_patched STREQUAL _radioify_libkss_kssplay_c_contents)
    file(WRITE "${_radioify_libkss_kssplay_c}" "${_radioify_libkss_kssplay_c_patched}")
  endif()
endif()
foreach(_radioify_libkss_target IN ITEMS emu2149 emu2212 emu2413 emu8950 emu76489 kmz80 kss kss2wav)
  if (TARGET ${_radioify_libkss_target})
    radioify_configure_external_target(${_radioify_libkss_target})
  endif()
endforeach()
radioify_add_external_include_dirs(
  ${libkss_SOURCE_DIR}/src
  ${libkss_SOURCE_DIR}/modules
)
target_link_libraries(radioify PRIVATE kss)
target_link_libraries(video_frame_step_smoke PRIVATE kss)

set(_radioify_disable_sdl2_defined OFF)
if (DEFINED CMAKE_DISABLE_FIND_PACKAGE_SDL2)
  set(_radioify_disable_sdl2_defined ON)
  set(_radioify_disable_sdl2_value ${CMAKE_DISABLE_FIND_PACKAGE_SDL2})
endif()
set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 TRUE)
set(ENABLE_UBSAN OFF CACHE BOOL "Disable libgme UBSan in Radioify builds" FORCE)
FetchContent_Declare(
  gme
  GIT_REPOSITORY https://github.com/ShiftMediaProject/Game-Music-Emu.git
  GIT_TAG 59e82746f6ff83df184554d17c7536e0ea609385
)
set(_radioify_gme_policy_default_cmp0074_defined OFF)
if (DEFINED CMAKE_POLICY_DEFAULT_CMP0074)
  set(_radioify_gme_policy_default_cmp0074_defined ON)
  set(_radioify_gme_policy_default_cmp0074_value "${CMAKE_POLICY_DEFAULT_CMP0074}")
endif()
set(CMAKE_POLICY_DEFAULT_CMP0074 NEW)
FetchContent_MakeAvailable(gme)
if (_radioify_gme_policy_default_cmp0074_defined)
  set(CMAKE_POLICY_DEFAULT_CMP0074 "${_radioify_gme_policy_default_cmp0074_value}")
else()
  unset(CMAKE_POLICY_DEFAULT_CMP0074)
endif()
foreach(_radioify_gme_target IN ITEMS gme gme_player demo demo_mem)
  if (TARGET ${_radioify_gme_target})
    radioify_configure_external_target(${_radioify_gme_target})
  endif()
endforeach()
radioify_add_external_include_dirs(${gme_SOURCE_DIR})
target_link_libraries(radioify PRIVATE gme)
target_link_libraries(video_frame_step_smoke PRIVATE gme)
if (_radioify_disable_sdl2_defined)
  set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 ${_radioify_disable_sdl2_value})
else()
  unset(CMAKE_DISABLE_FIND_PACKAGE_SDL2 CACHE)
  unset(CMAKE_DISABLE_FIND_PACKAGE_SDL2)
endif()
if (_radioify_warn_deprecated_defined)
  set(CMAKE_WARN_DEPRECATED ${_radioify_warn_deprecated_value} CACHE BOOL "Restore third-party CMake deprecation warning setting" FORCE)
  set(CMAKE_WARN_DEPRECATED ${_radioify_warn_deprecated_value})
else()
  unset(CMAKE_WARN_DEPRECATED CACHE)
  unset(CMAKE_WARN_DEPRECATED)
endif()

option(RADIOIFY_DISABLE_GSF_GPL "Disable GSF playback via GPL PlayGSF core" OFF)
target_compile_definitions(radioify_project_options INTERFACE
  RADIOIFY_DISABLE_GSF_GPL=$<BOOL:${RADIOIFY_DISABLE_GSF_GPL}>
)

if (NOT RADIOIFY_DISABLE_GSF_GPL)
  add_library(playgsf STATIC
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/gsf.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/GBA.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/Globals.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/Sound.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/Util.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/bios.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/snd_interp.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/unzip.cpp
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/memgzio.c
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA/psftag.c
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/libresample-0.1.3/src/resample.c
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/libresample-0.1.3/src/resamplesubs.c
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/libresample-0.1.3/src/filterkit.c
  )
  target_include_directories(playgsf PUBLIC
    ${CMAKE_SOURCE_DIR}/third_party/playgsf
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/libresample-0.1.3/include
  )
  radioify_add_external_include_dirs(
    ${CMAKE_SOURCE_DIR}/third_party/playgsf
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/libresample-0.1.3/include
  )
  target_link_libraries(playgsf PUBLIC ZLIB::ZLIB)
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(playgsf PRIVATE C_CORE)
  endif()
  radioify_configure_external_target(playgsf)
  target_link_libraries(radioify PRIVATE playgsf)
  target_link_libraries(video_frame_step_smoke PRIVATE playgsf)
endif()

set(LIBVGM_BUILD_PLAYER OFF CACHE BOOL "Build libvgm player app" FORCE)
set(LIBVGM_BUILD_VGM2WAV OFF CACHE BOOL "Build libvgm vgm2wav app" FORCE)
set(LIBVGM_BUILD_TESTS OFF CACHE BOOL "Build libvgm tests" FORCE)
set(LIBVGM_BUILD_LIBEMU ON CACHE BOOL "Build libvgm emu library" FORCE)
set(LIBVGM_BUILD_LIBPLAYER ON CACHE BOOL "Build libvgm player library" FORCE)
set(LIBVGM_BUILD_LIBAUDIO OFF CACHE BOOL "Build libvgm audio library" FORCE)
set(BUILD_PLAYER OFF CACHE BOOL "Build libvgm player app" FORCE)
set(BUILD_VGM2WAV OFF CACHE BOOL "Build libvgm vgm2wav app" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "Build libvgm tests" FORCE)
set(BUILD_LIBEMU ON CACHE BOOL "Build libvgm emu library" FORCE)
set(BUILD_LIBPLAYER ON CACHE BOOL "Build libvgm player library" FORCE)
set(BUILD_LIBAUDIO OFF CACHE BOOL "Build libvgm audio library" FORCE)
set(UTIL_CHARSET_CONV ON CACHE BOOL "Enable libvgm charset conversion" FORCE)
set(UTIL_CHARCNV_ICONV OFF CACHE BOOL "Disable iconv conversion in libvgm" FORCE)
set(UTIL_CHARCNV_WINAPI ON CACHE BOOL "Use WinAPI conversion in libvgm" FORCE)
set(USE_SANITIZERS OFF CACHE BOOL "Disable libvgm sanitizers in Radioify builds" FORCE)
FetchContent_Declare(
  libvgm
  GIT_REPOSITORY https://github.com/ValleyBell/libvgm.git
  GIT_TAG master
)
FetchContent_MakeAvailable(libvgm)
foreach(_radioify_libvgm_target IN ITEMS vgm-utils vgm-audio vgm-emu vgm-player vgmplayer vgmplayer-static vgm-player-static)
  if (TARGET ${_radioify_libvgm_target})
    radioify_configure_external_target(${_radioify_libvgm_target})
  endif()
endforeach()
radioify_add_external_include_dirs(${libvgm_SOURCE_DIR})
set(_radioify_libvgm_target "")
foreach(_candidate IN ITEMS vgm-player vgm-player-static vgmplayer vgmplayer-static
                           vgm_player libvgm)
  if (TARGET ${_candidate})
    set(_radioify_libvgm_target ${_candidate})
    break()
  endif()
endforeach()
if (NOT _radioify_libvgm_target)
  message(FATAL_ERROR "libvgm target not found; check libvgm build options.")
endif()
target_link_libraries(radioify PRIVATE ${_radioify_libvgm_target})
target_link_libraries(video_frame_step_smoke PRIVATE ${_radioify_libvgm_target})

set_target_properties(radioify PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${RADIOIFY_BUILD_RUNTIME_OUTPUT_DIRECTORY}"
)

foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(radioify PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${config_upper}
      "${RADIOIFY_BUILD_RUNTIME_OUTPUT_DIRECTORY}/${config}"
  )
endforeach()

if (WIN32)
  target_link_libraries(radioify PRIVATE dbghelp winmm mfplat mfreadwrite mfuuid ole32 windowscodecs runtimeobject d3d11 dxgi d3dcompiler user32 shell32 propsys comctl32)
  target_link_libraries(video_frame_step_smoke PRIVATE dbghelp winmm mfplat mfreadwrite mfuuid ole32 windowscodecs runtimeobject d3d11 dxgi d3dcompiler user32 shell32 propsys)
  target_link_libraries(timeline_preview_tests PRIVATE mfplat mfreadwrite mfuuid ole32 d3d11 dxgi user32 windowscodecs)
  target_link_libraries(scene_analysis_smoke PRIVATE mfplat mfreadwrite mfuuid ole32 d3d11 dxgi user32)
  target_link_libraries(video_edit_export_smoke PRIVATE mfplat mfreadwrite mfuuid ole32 bcrypt ws2_32)
endif()

if (WIN32 AND RADIOIFY_BUILD_WIN11_EXPLORER_INTEGRATION)
  add_subdirectory(windows/explorer)
endif()
