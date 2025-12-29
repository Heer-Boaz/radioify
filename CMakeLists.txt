cmake_minimum_required(VERSION 3.16)
project(radioify LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(radioify
  main.cpp
  consoleinput.cpp
  consolescreen.cpp
  m4adecoder.cpp
  radio.cpp
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist")
foreach(config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${config}" config_upper)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_SOURCE_DIR}/dist")
endforeach()

target_include_directories(radioify PRIVATE third_party)

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
  target_link_libraries(radioify PRIVATE winmm mfplat mfreadwrite mfuuid ole32)
endif()

if (MSVC)
  target_compile_options(radioify PRIVATE /EHsc)
endif()
