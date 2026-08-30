radioify_add_module_object_library(radioify_tool_radio_measurements
  ${CMAKE_CURRENT_SOURCE_DIR}/radio_measurements.cpp
)

radioify_add_module_object_library(radioify_tool_radio_node_physics
  ${CMAKE_CURRENT_SOURCE_DIR}/radio_node_physics.cpp
)

radioify_add_module_object_library(radioify_tool_radio_receiver_profile_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/radio_receiver_profile_tests.cpp
)

radioify_add_module_object_library(radioify_tool_radio_am_ingress_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/radio_am_ingress_tests.cpp
)

radioify_add_module_object_library(radioify_tool_audio_playback_pipeline_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/audio_playback_pipeline_tests.cpp
)

radioify_add_module_object_library(radioify_tool_audio_media_format_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/audio_media_format_tests.cpp
)

radioify_add_module_object_library(radioify_tool_loop_split_output_paths_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/loop_split_output_paths_tests.cpp
)

radioify_add_module_object_library(radioify_tool_melody_artifact_paths_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/melody_artifact_paths_tests.cpp
)

radioify_add_module_object_library(radioify_tool_ffmpeg_audio_decoder_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg_audio_decoder_tests.cpp
)

radioify_add_module_object_library(radioify_tool_frame_clipboard_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_clipboard_tests.cpp
)

radioify_add_module_object_library(radioify_tool_ascii_shader_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii_shader_tests.cpp
)

radioify_add_module_object_library(radioify_tool_shortcut_match_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/shortcut_match_tests.cpp
)

radioify_add_module_object_library(radioify_tool_playback_session_input_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_session_input_tests.cpp
)

radioify_add_module_object_library(radioify_tool_terminal_input_sequence_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/terminal_input_sequence_tests.cpp
)

radioify_add_module_object_library(radioify_tool_osd_timeline_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/osd_timeline_tests.cpp
)

radioify_add_module_object_library(radioify_tool_presentation_policy_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/presentation_policy_tests.cpp
)

radioify_add_module_object_library(radioify_tool_presentation_controller_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/presentation_controller_tests.cpp
)

radioify_add_module_object_library(radioify_tool_playback_exit_coordinator_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_exit_coordinator_tests.cpp
)

radioify_add_module_object_library(radioify_tool_deferred_media_handoff_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/deferred_media_handoff_tests.cpp
)

radioify_add_module_object_library(radioify_tool_video_output_color_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/video_output_color_tests.cpp
)

radioify_add_module_object_library(radioify_tool_video_frame_step_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/video_frame_step_smoke.cpp
)

radioify_add_module_object_library(radioify_tool_frame_step_snapshot_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_step_snapshot_tests.cpp
)

radioify_add_module_object_library(radioify_tool_timeline_preview_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/timeline_preview_tests.cpp
)

radioify_add_module_object_library(radioify_tool_video_edit_timeline_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/video_edit_timeline_tests.cpp
)

radioify_add_module_object_library(radioify_tool_video_edit_export_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/video_edit_export_smoke.cpp
)

radioify_add_module_object_library(radioify_tool_exporter_job_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/exporter_job_tests.cpp
)

radioify_add_module_object_library(radioify_tool_window_display_lifecycle_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/window_display_lifecycle_tests.cpp
)

radioify_add_module_object_library(radioify_tool_gpu_text_grid_composition_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu_text_grid_composition_tests.cpp
)

radioify_add_module_object_library(radioify_tool_waitable_signal_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/waitable_signal_tests.cpp
)

radioify_add_module_object_library(radioify_tool_pumpable_worker_thread_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/pumpable_worker_thread_tests.cpp
)

radioify_add_module_object_library(radioify_tool_windows_message_pump_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/windows_message_pump_tests.cpp
)

radioify_add_module_object_library(radioify_tool_thread_dispatch_queue_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/thread_dispatch_queue_tests.cpp
)

radioify_add_module_object_library(radioify_tool_window_input_queue_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/window_input_queue_tests.cpp
)

radioify_add_module_object_library(radioify_tool_open_file_requests_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/open_file_requests_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_windows_file_drop_acceptance_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/windows_file_drop_acceptance_tests.cpp
)

radioify_add_module_object_library(radioify_tool_shell_open_protocol_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/shell_open_protocol_tests.cpp
)

radioify_add_module_object_library(radioify_tool_utf8_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/utf8_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_subtitle_pixel_compositor_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle_pixel_compositor_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_subtitle_sidecar_discovery_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle_sidecar_discovery_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_navigation_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_navigation_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_key_input_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_key_input_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_directory_listing_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_directory_listing_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_content_view_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_content_view_tests.cpp
)

radioify_add_module_object_library(radioify_tool_popup_menu_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/popup_menu_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_media_menu_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_media_menu_tests.cpp
)

radioify_add_module_object_library(radioify_tool_command_palette_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/command_palette_tests.cpp
)

radioify_add_module_object_library(radioify_tool_shell_command_catalog_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/shell_command_catalog_tests.cpp
)

radioify_add_module_object_library(radioify_tool_shell_overlay_stack_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/shell_overlay_stack_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_action_strip_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_action_strip_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_chrome_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_chrome_tests.cpp
)

radioify_add_module_object_library(radioify_tool_browser_wake_schedule_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/browser_wake_schedule_tests.cpp
)

radioify_add_module_object_library(radioify_tool_playback_presentation_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_presentation_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_playback_control_session_mailbox_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_control_session_mailbox_tests.cpp
)

radioify_add_module_object_library(radioify_tool_window_input_events_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/window_input_events_tests.cpp
)

radioify_add_module_object_library(radioify_tool_melody_visualization_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/melody_visualization_tests.cpp
)

radioify_add_module_object_library(radioify_tool_tui_theme_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/tui_theme_tests.cpp
)

radioify_add_module_object_library(radioify_tool_playback_queue_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_queue_tests.cpp
)

radioify_add_module_object_library(radioify_tool_image_viewer_sequence_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/image_viewer_sequence_tests.cpp
)

radioify_add_module_object_library(radioify_tool_media_activation_plan_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/media_activation_plan_tests.cpp
)

radioify_add_module_object_library(radioify_tool_indexed_transcript_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/indexed_transcript_tests.cpp
)

radioify_add_module_object_library(radioify_tool_ui_footer_layout_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/ui_footer_layout_tests.cpp
)

radioify_add_module_object_library(radioify_tool_ui_viewport_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/ui_viewport_tests.cpp
)

radioify_add_module_object_library(radioify_tool_ui_input_pump_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/ui_input_pump_tests.cpp
)

radioify_add_module_object_library(radioify_tool_media_task_panel_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/media_task_panel_tests.cpp
)

radioify_add_module_object_library(radioify_tool_application_exit_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/application_exit_tests.cpp
)

radioify_add_module_object_library(radioify_tool_playback_dialogs_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_dialogs_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_playback_session_bootstrap_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_session_bootstrap_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_playback_shutdown_sequence_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/playback_shutdown_sequence_tests.cpp
)

radioify_add_module_object_library(radioify_tool_subtitle_loader_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle_loader_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_media_action_confirmation_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/media_action_confirmation_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_overlay_control_pointer_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/overlay_control_pointer_tests.cpp
)

radioify_add_module_object_library(radioify_tool_media_action_catalog_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/media_action_catalog_tests.cpp
)

radioify_add_module_object_library(radioify_tool_subtitle_generation_job_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle_generation_job_tests.cpp
)

radioify_add_module_object_library(
  radioify_tool_media_processing_coordinator_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/media_processing_coordinator_tests.cpp
)

radioify_add_module_object_library(radioify_tool_file_output_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/file_output_tests.cpp
)

radioify_add_module_object_library(radioify_tool_media_export_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/media_export_tests.cpp
)

radioify_add_module_object_library(radioify_tool_audio_export_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/audio_export_smoke.cpp
)

radioify_add_module_object_library(radioify_tool_whisper_gpu_contract_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/whisper_gpu_contract_tests.cpp
)

radioify_add_module_object_library(radioify_tool_indexed_transcript_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/indexed_transcript_smoke.cpp
)

radioify_add_module_object_library(radioify_tool_scene_analysis_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/scene_analysis_tests.cpp
)

radioify_add_module_object_library(radioify_tool_scene_analysis_job_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/scene_analysis_job_tests.cpp
)

radioify_add_module_object_library(radioify_tool_scene_suggestion_review_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/scene_suggestion_review_tests.cpp
)

radioify_add_module_object_library(radioify_tool_scene_analysis_smoke
  ${CMAKE_CURRENT_SOURCE_DIR}/scene_analysis_smoke.cpp
)

if (RADIOIFY_HAS_AUDIO_SEPARATION)
  radioify_add_module_object_library(radioify_tool_audio_separation_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_separation_tests.cpp
  )
  set_target_properties(radioify_tool_audio_separation_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )

  radioify_add_module_object_library(radioify_tool_audio_separation_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_separation_smoke.cpp
  )

  radioify_add_module_object_library(
    radioify_tool_audio_separation_scheduler_smoke
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_separation_scheduler_smoke.cpp
  )

  radioify_add_module_object_library(
    radioify_tool_audio_separation_model_stress
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_separation_model_stress.cpp
  )
  set_target_properties(
    radioify_tool_audio_separation_model_stress PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )

  radioify_add_module_object_library(
    radioify_tool_audio_separation_spectral_stress
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_separation_spectral_stress.cpp
  )
  set_target_properties(
    radioify_tool_audio_separation_spectral_stress PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
  )
endif()

radioify_add_module_object_library(radioify_tool_windows_console_window_tests
  ${CMAKE_CURRENT_SOURCE_DIR}/windows_console_window_tests.cpp
)
