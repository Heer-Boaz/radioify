add_subdirectory(ui)

radioify_add_module_object_library(radioify_tui
  ${CMAKE_CURRENT_SOURCE_DIR}/audio_picture_in_picture_window.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_grid_index.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/consoleinput.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/consolescreen.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/image_viewer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/image_viewer_sequence.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/media_activation_plan.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/media_coordinator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/terminal_input_sequence.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/terminal_cell_metrics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tui_export.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tui_theme.cpp
)
