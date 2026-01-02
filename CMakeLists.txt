cmake_minimum_required(VERSION 3.16)
project(radioify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(radioify
  main.cpp
  asciiart.cpp
  consoleinput.cpp
  consolescreen.cpp
  ffmpegaudio.cpp
  m4adecoder.cpp
  videodecoder.cpp
  radio.cpp
)

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
  target_link_libraries(radioify PRIVATE winmm mfplat mfreadwrite mfuuid ole32 windowscodecs runtimeobject d3d11 dxgi)
endif()

if (MSVC)
  target_compile_options(radioify PRIVATE /EHsc)
endif()
