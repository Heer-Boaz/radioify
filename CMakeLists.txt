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
