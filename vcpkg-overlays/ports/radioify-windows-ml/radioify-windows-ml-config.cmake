if(TARGET WindowsML::OnnxRuntime AND
   TARGET WindowsML::ExecutionProviderCatalog AND
   TARGET WindowsML::Runtime)
  return()
endif()
if(TARGET WindowsML::OnnxRuntime OR
   TARGET WindowsML::ExecutionProviderCatalog OR
   TARGET WindowsML::Runtime)
  message(FATAL_ERROR
    "radioify-windows-ml was only partially imported into this build")
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

add_library(WindowsML::ExecutionProviderCatalog SHARED IMPORTED)
set_target_properties(WindowsML::ExecutionProviderCatalog PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES
    "${_radioify_windows_ml_prefix}/include"
  INTERFACE_LINK_LIBRARIES
    WindowsML::OnnxRuntime
  IMPORTED_IMPLIB
    "${_radioify_windows_ml_prefix}/lib/Microsoft.Windows.AI.MachineLearning.lib"
  IMPORTED_LOCATION
    "${_radioify_windows_ml_prefix}/bin/Microsoft.Windows.AI.MachineLearning.dll"
)

add_library(WindowsML::Runtime INTERFACE IMPORTED)
set_target_properties(WindowsML::Runtime PROPERTIES
  INTERFACE_LINK_LIBRARIES
    "WindowsML::OnnxRuntime;WindowsML::ExecutionProviderCatalog"
)

# DirectML has no import library. ONNX Runtime loads this DLL dynamically.
add_library(WindowsML::DirectML INTERFACE IMPORTED)
set(WINML_DIRECTML_DLL
    "${_radioify_windows_ml_prefix}/bin/DirectML.dll")

unset(_radioify_windows_ml_prefix)
