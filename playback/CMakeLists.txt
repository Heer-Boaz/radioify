add_subdirectory(video)

radioify_add_module_object_library(radioify_playback
  ${CMAKE_CURRENT_SOURCE_DIR}/media/artwork_catalog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/media/metadata_catalog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/media/track_catalog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/system_media_transport/host_window.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/system_media_transport/controls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/session.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/bootstrap.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/core.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/host.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/file_drop.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/handoff.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/input.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/loop.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/log.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/output.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/presentation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/presentation_controller.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/runtime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/window_presentation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/overlay/overlay.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/overlay/subtitle_effects.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/subtitles.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/frame_output_ascii.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/frame_output_console.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/screen_renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/presenter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/video_pipeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/mini_player_tui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window_presenter.cpp
)

set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/system_media_transport/controls.cpp
  PROPERTIES COMPILE_FLAGS "/std:c++20"
)
