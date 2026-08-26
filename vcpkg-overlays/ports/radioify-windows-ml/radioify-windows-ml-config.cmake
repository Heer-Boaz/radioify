if(TARGET WindowsML::OnnxRuntime)
  return()
endif()

get_filename_component(_radioify_windows_ml_prefix
  "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)

add_library(WindowsML::OnnxRuntime SHARED IMPORTED)
set_target_properties(WindowsML::OnnxRuntime PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES
    "${_radioify_windows_ml_prefix}/include/winml"
  IMPORTED_IMPLIB
    "${_radioify_windows_ml_prefix}/lib/onnxruntime.lib"
  IMPORTED_LOCATION
    "${_radioify_windows_ml_prefix}/bin/onnxruntime.dll"
)

# DirectML has no import library. ONNX Runtime loads this DLL dynamically.
add_library(WindowsML::DirectML INTERFACE IMPORTED)
set(WINML_DIRECTML_DLL
    "${_radioify_windows_ml_prefix}/bin/DirectML.dll")

unset(_radioify_windows_ml_prefix)
