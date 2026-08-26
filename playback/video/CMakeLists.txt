radioify_add_module_object_library(radioify_image_codec
  ${CMAKE_CURRENT_SOURCE_DIR}/image_wic.cpp
)

radioify_add_module_object_library(radioify_ascii_cpu
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/asciiart.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/asciiart_layout.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/luminance_stats.cpp
)

radioify_add_module_object_library(radioify_ascii_gpu
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/asciiart_gpu.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu/gpu_shared.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu/videoprocessor.cpp
)

radioify_add_module_object_library(radioify_video_playback
  ${CMAKE_CURRENT_SOURCE_DIR}/composition/preview_cache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/composition/render_plan.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/audio/clock_reacquire.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/audio/output_clock_source.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/audio/output_timeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/audio/track_switch_timeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/timing/main_clock.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/control/serial.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/state/machine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/timing/sync.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/timing/timeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_cursor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_step_prefetch.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_step_prefetch_mapping.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_step_snapshot.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_step_source_cache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_step_seek.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/sequence.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/decoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/player.cpp
)

radioify_add_module_object_library(radioify_timeline_preview
  ${CMAKE_CURRENT_SOURCE_DIR}/timeline_preview.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/timeline_preview_cache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/timeline_preview_decoder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/timeline_preview_model.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/timeline_preview_policy.cpp
)

radioify_add_module_object_library(radioify_video_sidecar_identity
  ${CMAKE_CURRENT_SOURCE_DIR}/sidecar_identity.cpp
)

radioify_add_module_object_library(radioify_transcript_artifact
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/artifact.cpp
)

radioify_add_module_object_library(radioify_transcript_document
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/document.cpp
)

radioify_add_module_object_library(radioify_transcript_cue_semantics
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/cue_semantics.cpp
)

radioify_add_module_object_library(radioify_transcript_device_policy
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/device_selection.cpp
)

radioify_add_module_object_library(radioify_transcript_job
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/generation_job.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/generation_job_default.cpp
)

radioify_add_module_object_library(radioify_transcript_engine
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/subtitle_cues.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/transcriber.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/whisper_engine.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/transcript/whisper_model_config.cpp
)

radioify_add_module_object_library(radioify_scene_analysis
  ${CMAKE_CURRENT_SOURCE_DIR}/analysis/scene_analysis.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/analysis/scene_analysis_cache.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/analysis/scene_analyzer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/analysis/scene_analysis_job.cpp
)

radioify_add_module_object_library(radioify_video
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/asciiart.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/asciiart_layout.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/ascii/asciiart_gpu.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu/gpu_shared.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/gpu/videoprocessor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle/ass/parser.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle/ass/script.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle/ass/bitmap_renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle/font_attachments.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle/manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/subtitle/sidecar_discovery.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/enhancement/pipeline.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/enhancement/nvidia_d3d11.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/frame_cache/update.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/luminance_stats.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/text_grid_bitmap_renderer.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/subtitle_pixel_compositor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/frame_clipboard.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/video_output_color.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/window.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/file_drop.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/gpu_textgrid.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/input_controller.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/input_events.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/input_queue.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/messages.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/display_lifecycle.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/framebuffer/window/textgrid.cpp
)
