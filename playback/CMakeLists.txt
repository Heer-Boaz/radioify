add_subdirectory(video)

radioify_add_module_object_library(radioify_playback
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_media_artwork_catalog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_media_metadata_catalog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_track_catalog.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/system_media_transport_host_window.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/system_media_transport_controls.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_bootstrap.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_core.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_host.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_file_drop.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_handoff.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_input.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_loop.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_log.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_output.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_presentation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_presentation_controller.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_session_runtime.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/session/playback_window_presentation.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/overlay/playback_overlay.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/overlay/playback_subtitle_effects.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/playback_ascii_subtitles.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/playback_frame_output_ascii.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/playback_frame_output_console.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/playback_screen_renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/playback_framebuffer_presenter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/playback_framebuffer_video_pipeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/playback_mini_player_tui.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/playback_window_presenter.cpp
)

set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/system_media_transport_controls.cpp
  PROPERTIES COMPILE_FLAGS "/std:c++20"
)
