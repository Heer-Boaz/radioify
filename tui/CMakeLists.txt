add_subdirectory(ui)

radioify_add_module_object_library(radioify_tui
  ${CMAKE_CURRENT_SOURCE_DIR}/audio_mini_player.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_grid_index.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/consoleinput.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/consolescreen.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/input_file_drop.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/terminal_input_sequence.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/terminal_cell_metrics.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/tui_export.cpp
)
