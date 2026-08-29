set(_radioify_audio_separation_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/artifact.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/inference_backend.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/job.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/operation.cpp
)
if (RADIOIFY_HAS_AUDIO_SEPARATION)
  list(APPEND _radioify_audio_separation_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/mask_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/separator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/spectral_transform.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/windows_ml_backend.cpp
  )
else()
  list(APPEND _radioify_audio_separation_sources
    ${CMAKE_CURRENT_SOURCE_DIR}/separator_unavailable.cpp
  )
endif()

radioify_add_module_object_library(radioify_audio_separation
  ${_radioify_audio_separation_sources}
)

# The current Windows ML C++ API and std::numbers used by the spectral code
# require C++20. Keep that requirement local to this module.
set_target_properties(radioify_audio_separation PROPERTIES
  CXX_STANDARD 20
  CXX_STANDARD_REQUIRED ON
)
