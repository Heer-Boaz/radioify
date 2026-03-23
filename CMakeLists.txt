cmake_minimum_required(VERSION 3.16)
if (POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
project(radioify LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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
option(RADIOIFY_DOWNLOAD_NEURAL_PITCH_MODEL "Download neural pitch model automatically when missing" ON)
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

add_executable(radioify
  app_common.cpp
  crash_handler.cpp
  main.cpp
  tui.cpp
  asciiart.cpp
  asciiart_gpu.cpp
  audioplayback.cpp
  melodyanalysiscache.cpp
  melodyanalyzer.cpp
  neuralpitch.cpp
  browsermeta.cpp
  consoleinput.cpp
  ui_inputlogic.cpp
  consolescreen.cpp
  ffmpegaudio.cpp
  flacaudio.cpp
  gpu_shared.cpp
  gmeaudio.cpp
  gsfaudio.cpp
  midiaudio.cpp
  vgmaudio.cpp
  kssaudio.cpp
  psfaudio.cpp
  m4adecoder.cpp
  optionsbrowser.cpp
  tracklist.cpp
  player.cpp
  queues.cpp
  taskgate.cpp
  ui_helpers.cpp
  videoplayback.cpp
  videodecoder.cpp
  videowindow.cpp
  videowindow_tui.cpp
  videoprocessor.cpp
  radio_settings.cpp
  playback_frame_output_ascii.cpp
  playback_frame_output_console.cpp
  subtitle_manager.cpp
  calibration_report.cpp
  radio.cpp
  radiopreview.cpp
  loopsplit.cpp
  loopsplit_cli.cpp
  loopsplit_ui.cpp
  )

target_compile_definitions(radioify PRIVATE
  RADIOIFY_ENABLE_TIMING_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_TIMING_LOG}>,1,0>
  RADIOIFY_ENABLE_VIDEO_ERROR_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_VIDEO_ERROR_LOG}>,1,0>
  RADIOIFY_ENABLE_FFMPEG_ERROR_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_FFMPEG_ERROR_LOG}>,1,0>
  RADIOIFY_ENABLE_STAGING_UPLOAD=$<IF:$<BOOL:${RADIOIFY_ENABLE_STAGING_UPLOAD}>,1,0>
)

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

set(RADIOIFY_SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders")
set(RADIOIFY_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(RADIOIFY_EMBED_BINARY_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/EmbedBinary.cmake")

set(RADIOIFY_COMPUTE_SHADER_HLSL "${RADIOIFY_SHADER_DIR}/asciiart_gpu.hlsl")
set(RADIOIFY_STATS_SHADER_HLSL "${RADIOIFY_SHADER_DIR}/asciiart_stats.hlsl")
set(RADIOIFY_VIDEOWINDOW_SHADER_HLSL "${RADIOIFY_SHADER_DIR}/videowindow_render.hlsl")
set(RADIOIFY_VIDEOWINDOW_OVERLAY_HLSL "${RADIOIFY_SHADER_DIR}/videowindow_overlay.hlsl")
set(RADIOIFY_COMPUTE_SHADER_CSO_MAIN
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main_nv12.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_BG_CLAMP
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_bg_clamp.cso")
set(RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_sync_history.cso")
set(RADIOIFY_STATS_SHADER_CSO_RGBA
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_rgba.cso")
set(RADIOIFY_STATS_SHADER_CSO_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_nv12.cso")
set(RADIOIFY_VIDEOWINDOW_SHADER_CSO_VS
    "${RADIOIFY_GENERATED_DIR}/videowindow_vs.cso")
set(RADIOIFY_VIDEOWINDOW_SHADER_CSO_PS
    "${RADIOIFY_GENERATED_DIR}/videowindow_ps.cso")
set(RADIOIFY_VIDEOWINDOW_OVERLAY_CSO_PS
    "${RADIOIFY_GENERATED_DIR}/videowindow_ps_ui.cso")

set(RADIOIFY_COMPUTE_SHADER_HEADER_MAIN
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_MAIN_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_main_nv12.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_BG_CLAMP
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_bg_clamp.h")
set(RADIOIFY_COMPUTE_SHADER_HEADER_SYNC_HISTORY
    "${RADIOIFY_GENERATED_DIR}/asciiart_gpu_cs_sync_history.h")
set(RADIOIFY_STATS_SHADER_HEADER_RGBA
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_rgba.h")
set(RADIOIFY_STATS_SHADER_HEADER_NV12
    "${RADIOIFY_GENERATED_DIR}/asciiart_stats_cs_nv12.h")
set(RADIOIFY_VIDEOWINDOW_SHADER_HEADER_VS
    "${RADIOIFY_GENERATED_DIR}/videowindow_vs.h")
set(RADIOIFY_VIDEOWINDOW_SHADER_HEADER_PS
    "${RADIOIFY_GENERATED_DIR}/videowindow_ps.h")
set(RADIOIFY_VIDEOWINDOW_OVERLAY_HEADER_PS
    "${RADIOIFY_GENERATED_DIR}/videowindow_ps_ui.h")

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
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSMain
            /D NV12_INPUT=1
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_MAIN_NV12}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_STATS_SHADER_CSO_RGBA}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSStats
            /D RGBA_INPUT=1
            /Fo "${RADIOIFY_STATS_SHADER_CSO_RGBA}"
            "${RADIOIFY_STATS_SHADER_HLSL}"
    DEPENDS "${RADIOIFY_STATS_SHADER_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_STATS_SHADER_CSO_NV12}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSStats
            /D NV12_INPUT=1
            /Fo "${RADIOIFY_STATS_SHADER_CSO_NV12}"
            "${RADIOIFY_STATS_SHADER_HLSL}"
    DEPENDS "${RADIOIFY_STATS_SHADER_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_BG_CLAMP}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSBgClamp
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_BG_CLAMP}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags} /E CSSyncHistory
            /Fo "${RADIOIFY_COMPUTE_SHADER_CSO_SYNC_HISTORY}"
            "${RADIOIFY_COMPUTE_SHADER_HLSL}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_HLSL}"
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
    OUTPUT "${RADIOIFY_VIDEOWINDOW_SHADER_CSO_VS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags_vs} /E VS
            /Fo "${RADIOIFY_VIDEOWINDOW_SHADER_CSO_VS}"
            "${RADIOIFY_VIDEOWINDOW_SHADER_HLSL}"
    DEPENDS "${RADIOIFY_VIDEOWINDOW_SHADER_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_VIDEOWINDOW_SHADER_CSO_PS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags_ps} /E PS
            /Fo "${RADIOIFY_VIDEOWINDOW_SHADER_CSO_PS}"
            "${RADIOIFY_VIDEOWINDOW_SHADER_HLSL}"
    DEPENDS "${RADIOIFY_VIDEOWINDOW_SHADER_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_VIDEOWINDOW_OVERLAY_CSO_PS}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${RADIOIFY_GENERATED_DIR}"
    COMMAND "${RADIOIFY_FXC_EXE}" ${_radioify_fxc_flags_ps} /E PS_UI
            /Fo "${RADIOIFY_VIDEOWINDOW_OVERLAY_CSO_PS}"
            "${RADIOIFY_VIDEOWINDOW_OVERLAY_HLSL}"
    DEPENDS "${RADIOIFY_VIDEOWINDOW_OVERLAY_HLSL}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_COMPUTE_SHADER_HEADER_BG_CLAMP}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_COMPUTE_SHADER_CSO_BG_CLAMP}
            -DOUTPUT=${RADIOIFY_COMPUTE_SHADER_HEADER_BG_CLAMP}
            -DVAR=kComputeShaderCsBgClamp
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_COMPUTE_SHADER_CSO_BG_CLAMP}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
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
    OUTPUT "${RADIOIFY_VIDEOWINDOW_SHADER_HEADER_VS}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_VIDEOWINDOW_SHADER_CSO_VS}
            -DOUTPUT=${RADIOIFY_VIDEOWINDOW_SHADER_HEADER_VS}
            -DVAR=kVideoWindowVs
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_VIDEOWINDOW_SHADER_CSO_VS}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_VIDEOWINDOW_SHADER_HEADER_PS}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_VIDEOWINDOW_SHADER_CSO_PS}
            -DOUTPUT=${RADIOIFY_VIDEOWINDOW_SHADER_HEADER_PS}
            -DVAR=kVideoWindowPs
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_VIDEOWINDOW_SHADER_CSO_PS}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  add_custom_command(
    OUTPUT "${RADIOIFY_VIDEOWINDOW_OVERLAY_HEADER_PS}"
    COMMAND ${CMAKE_COMMAND}
            -DINPUT=${RADIOIFY_VIDEOWINDOW_OVERLAY_CSO_PS}
            -DOUTPUT=${RADIOIFY_VIDEOWINDOW_OVERLAY_HEADER_PS}
            -DVAR=kVideoWindowPsUi
            -P "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    DEPENDS "${RADIOIFY_VIDEOWINDOW_OVERLAY_CSO_PS}" "${RADIOIFY_EMBED_BINARY_SCRIPT}"
    VERBATIM
  )

  set(RADIOIFY_SHADER_HEADERS
    "${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_MAIN_NV12}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_BG_CLAMP}"
    "${RADIOIFY_COMPUTE_SHADER_HEADER_SYNC_HISTORY}"
    "${RADIOIFY_STATS_SHADER_HEADER_RGBA}"
    "${RADIOIFY_STATS_SHADER_HEADER_NV12}"
    "${RADIOIFY_VIDEOWINDOW_SHADER_HEADER_VS}"
    "${RADIOIFY_VIDEOWINDOW_SHADER_HEADER_PS}"
    "${RADIOIFY_VIDEOWINDOW_OVERLAY_HEADER_PS}"
  )

  add_custom_target(radioify_shader_blobs
    DEPENDS ${RADIOIFY_SHADER_HEADERS}
  )

  add_dependencies(radioify radioify_shader_blobs)
  target_include_directories(radioify PRIVATE "${RADIOIFY_GENERATED_DIR}")
  target_sources(radioify PRIVATE ${RADIOIFY_SHADER_HEADERS})
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")
foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_SOURCE_DIR}/dist")
endforeach()

target_include_directories(radioify PRIVATE third_party)

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

target_include_directories(radioify PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_include_directories(radioify PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
if (FFMPEG_LIBRARY_DIRS)
  target_link_directories(radioify PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(radioify PRIVATE ${FFMPEG_LIBRARIES})

set(RADIOIFY_HAS_LIBASS OFF)
set(_radioify_libass_include_dirs "")
set(_radioify_libass_libraries "")
set(_radioify_libass_hints "")

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
  find_library(RADIOIFY_LIBASS_LIBRARY
    NAMES ass libass
    HINTS ENV LIBASS_DIR ENV LIBASS_ROOT ${_radioify_libass_hints}
    PATH_SUFFIXES lib
  )
  if (RADIOIFY_LIBASS_INCLUDE_DIR AND RADIOIFY_LIBASS_LIBRARY)
    set(RADIOIFY_HAS_LIBASS ON)
    set(_radioify_libass_include_dirs ${RADIOIFY_LIBASS_INCLUDE_DIR})
    set(_radioify_libass_libraries ${RADIOIFY_LIBASS_LIBRARY})

    if (DEFINED VCPKG_TARGET_TRIPLET AND VCPKG_TARGET_TRIPLET MATCHES "-static")
      set(_radioify_libass_dep_libraries "")
      foreach(_radioify_ass_dep freetype harfbuzz fribidi brotlidec brotlicommon bz2 libpng16 png16 zlib)
        string(TOUPPER "${_radioify_ass_dep}" _radioify_ass_dep_upper)
        set(_radioify_ass_dep_var "RADIOIFY_LIBASS_DEP_${_radioify_ass_dep_upper}")
        find_library(${_radioify_ass_dep_var}
          NAMES ${_radioify_ass_dep}
          HINTS ${_radioify_libass_hints}
          PATH_SUFFIXES lib
        )
        if (${_radioify_ass_dep_var})
          list(APPEND _radioify_libass_dep_libraries ${${_radioify_ass_dep_var}})
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
target_include_directories(radioify PRIVATE ${_radioify_libass_include_dirs})
if (RADIOIFY_LIBASS_LIBRARY_DIRS)
  target_link_directories(radioify PRIVATE ${RADIOIFY_LIBASS_LIBRARY_DIRS})
endif()
target_link_libraries(radioify PRIVATE ${_radioify_libass_libraries})
message(STATUS "libass found: ASS advanced renderer enabled")

target_compile_definitions(radioify PRIVATE
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
      target_include_directories(radioify PRIVATE ${ONNXRUNTIME_INCLUDE_DIR})
      target_link_libraries(radioify PRIVATE ${ONNXRUNTIME_LIBRARY})
      message(STATUS "Neural pitch: ONNX Runtime enabled via manual lookup.")
    else()
      message(FATAL_ERROR "Melody analysis requested but ONNX Runtime not found (tried CMake package and manual lookup).")
    endif()
  endif()
endif()

target_compile_definitions(radioify PRIVATE
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
target_include_directories(psfcore PUBLIC
  ${psf2wav_SOURCE_DIR}/highly_experimental
)
target_compile_definitions(psfcore PRIVATE EMU_COMPILE EMU_LITTLE_ENDIAN)
target_link_libraries(psfcore PUBLIC ZLIB::ZLIB)
target_link_libraries(radioify PRIVATE psfcore)
target_link_libraries(radioify PRIVATE sexypsf)

set(_radioify_warn_deprecated_defined OFF)
if (DEFINED CMAKE_WARN_DEPRECATED)
  set(_radioify_warn_deprecated_defined ON)
  set(_radioify_warn_deprecated_value ${CMAKE_WARN_DEPRECATED})
endif()
set(CMAKE_WARN_DEPRECATED OFF)
FetchContent_Declare(
  libkss
  GIT_REPOSITORY https://github.com/digital-sound-antiques/libkss.git
  GIT_TAG 1.2.1
  GIT_SUBMODULES_RECURSE TRUE
)
FetchContent_MakeAvailable(libkss)
target_include_directories(radioify PRIVATE ${libkss_SOURCE_DIR}/src)
target_include_directories(radioify PRIVATE ${libkss_SOURCE_DIR}/modules)
target_link_libraries(radioify PRIVATE kss)

set(_radioify_disable_sdl2_defined OFF)
if (DEFINED CMAKE_DISABLE_FIND_PACKAGE_SDL2)
  set(_radioify_disable_sdl2_defined ON)
  set(_radioify_disable_sdl2_value ${CMAKE_DISABLE_FIND_PACKAGE_SDL2})
endif()
set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 TRUE)
FetchContent_Declare(
  gme
  GIT_REPOSITORY https://github.com/ShiftMediaProject/Game-Music-Emu.git
  GIT_TAG 59e82746f6ff83df184554d17c7536e0ea609385
)
FetchContent_MakeAvailable(gme)
target_include_directories(radioify PRIVATE ${gme_SOURCE_DIR})
target_link_libraries(radioify PRIVATE gme)
if (_radioify_disable_sdl2_defined)
  set(CMAKE_DISABLE_FIND_PACKAGE_SDL2 ${_radioify_disable_sdl2_value})
else()
  unset(CMAKE_DISABLE_FIND_PACKAGE_SDL2 CACHE)
  unset(CMAKE_DISABLE_FIND_PACKAGE_SDL2)
endif()
if (_radioify_warn_deprecated_defined)
  set(CMAKE_WARN_DEPRECATED ${_radioify_warn_deprecated_value})
else()
  unset(CMAKE_WARN_DEPRECATED CACHE)
  unset(CMAKE_WARN_DEPRECATED)
endif()

option(RADIOIFY_DISABLE_GSF_GPL "Disable GSF playback via GPL PlayGSF core" OFF)
target_compile_definitions(radioify PRIVATE RADIOIFY_DISABLE_GSF_GPL=$<BOOL:${RADIOIFY_DISABLE_GSF_GPL}>)

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
  target_include_directories(radioify PRIVATE
    ${CMAKE_SOURCE_DIR}/third_party/playgsf
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/VBA
    ${CMAKE_SOURCE_DIR}/third_party/playgsf/libresample-0.1.3/include
  )
  target_link_libraries(playgsf PUBLIC ZLIB::ZLIB)
  if (CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_compile_definitions(playgsf PRIVATE C_CORE)
  endif()
  if (MSVC)
    target_compile_options(playgsf PRIVATE /W0)
    target_compile_definitions(playgsf PRIVATE _CRT_SECURE_NO_WARNINGS)
  else()
    target_compile_options(playgsf PRIVATE -w)
  endif()
  target_link_libraries(radioify PRIVATE playgsf)
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
FetchContent_Declare(
  libvgm
  GIT_REPOSITORY https://github.com/ValleyBell/libvgm.git
  GIT_TAG master
)
FetchContent_MakeAvailable(libvgm)
target_include_directories(radioify PRIVATE ${libvgm_SOURCE_DIR})
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

set_target_properties(radioify PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
)

foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set_target_properties(radioify PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_SOURCE_DIR}/dist"
  )
endforeach()

if (WIN32)
  target_link_libraries(radioify PRIVATE dbghelp winmm mfplat mfreadwrite mfuuid ole32 windowscodecs runtimeobject d3d11 dxgi d3dcompiler user32)
endif()

if (MSVC)
  target_compile_options(radioify PRIVATE /EHsc)
endif()
