cmake_minimum_required(VERSION 3.16)
project(radioify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(RADIOIFY_ENABLE_TIMING_LOG "Enable timing log output" OFF)
option(RADIOIFY_ENABLE_VIDEO_ERROR_LOG "Enable video error log output" OFF)
option(RADIOIFY_ENABLE_FFMPEG_ERROR_LOG "Enable ffmpeg error log output" OFF)
option(RADIOIFY_ENABLE_STAGING_UPLOAD "Enable staging-based texture uploads to avoid implicit GPU sync" OFF)

add_executable(radioify
  main.cpp
  asciiart.cpp
  asciiart_gpu.cpp
  audioplayback.cpp
  browsermeta.cpp
  consoleinput.cpp
  consolescreen.cpp
  ffmpegaudio.cpp
  gpu_shared.cpp
  gmeaudio.cpp
  kssaudio.cpp
  m4adecoder.cpp
  player.cpp
  queues.cpp
  taskgate.cpp
  ui_helpers.cpp
  subtitles.cpp
  videoplayback.cpp
  videodecoder.cpp
  videowindow.cpp
  videoprocessor.cpp
  radio.cpp
)

target_compile_definitions(radioify PRIVATE
  RADIOIFY_ENABLE_TIMING_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_TIMING_LOG}>,1,0>
  RADIOIFY_ENABLE_VIDEO_ERROR_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_VIDEO_ERROR_LOG}>,1,0>
  RADIOIFY_ENABLE_FFMPEG_ERROR_LOG=$<IF:$<BOOL:${RADIOIFY_ENABLE_FFMPEG_ERROR_LOG}>,1,0>
  RADIOIFY_ENABLE_STAGING_UPLOAD=$<IF:$<BOOL:${RADIOIFY_ENABLE_STAGING_UPLOAD}>,1,0>
)

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
  find_path(FFMPEG_INCLUDE_DIR
    NAMES libavformat/avformat.h
    HINTS ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES include
  )
  find_library(AVFORMAT_LIBRARY NAMES avformat
    HINTS ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(AVCODEC_LIBRARY NAMES avcodec
    HINTS ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(AVUTIL_LIBRARY NAMES avutil
    HINTS ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(SWSCALE_LIBRARY NAMES swscale
    HINTS ENV FFMPEG_DIR ENV FFMPEG_ROOT
    PATH_SUFFIXES lib
  )
  find_library(SWRESAMPLE_LIBRARY NAMES swresample
    HINTS ENV FFMPEG_DIR ENV FFMPEG_ROOT
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
if (FFMPEG_LIBRARY_DIRS)
  target_link_directories(radioify PRIVATE ${FFMPEG_LIBRARY_DIRS})
endif()
target_link_libraries(radioify PRIVATE ${FFMPEG_LIBRARIES})
include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libs" FORCE)
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
FetchContent_Declare(
  gme
  GIT_REPOSITORY https://github.com/ShiftMediaProject/Game-Music-Emu.git
  GIT_TAG 59e82746f6ff83df184554d17c7536e0ea609385
)
FetchContent_MakeAvailable(gme)
target_include_directories(radioify PRIVATE ${gme_SOURCE_DIR})
target_link_libraries(radioify PRIVATE gme)

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
  target_link_libraries(radioify PRIVATE winmm mfplat mfreadwrite mfuuid ole32 windowscodecs runtimeobject d3d11 dxgi d3dcompiler user32)
endif()

if (MSVC)
  target_compile_options(radioify PRIVATE /EHsc)
  # Resolve LNK4098: defaultlib 'LIBCMT' conflicts with use of other libs
  target_link_options(radioify PRIVATE /NODEFAULTLIB:LIBCMT)
endif()
