# Radioify llama.cpp overlay

This runtime-only port pins llama.cpp v0.4.0 at the revision in `portfile.cmake`.
The ggml overlay uses the tensor subtree of that same revision. The upgrade is
needed for native Qwen3-VL temporal frame merging and explicit vision-device
selection; Radioify does not implement these model operations itself.

- The package supplies `llama::llama` and `llama::mtmd`, including mtmd's hash
  archive and thread/tensor dependencies. vcpkg owns debug/release library
  lookup. The installed package publishes its source revision for cache identity.
- Upstream mtmd's miniaudio decoder is now private (`MA_API static`) with device
  I/O disabled. It neither exports conflicting symbols nor owns Radioify's
  playback device. No custom miniaudio integration patch remains.
- Upstream `mtmd_context_params.device` selects the projector device explicitly.
  The old private backend-query extension is no longer needed.
- Radioify owns FFmpeg decoding, selected streams and frame timestamps.
  `MTMD_VIDEO=OFF` disables upstream's alternate FFmpeg reader, not temporal
  video tokenization. Lazy mergeable bitmaps supply native video input.
- Command-line tools, servers, OpenSSL downloads and example applications are
  not part of this product dependency; Radioify owns installation and its
  isolated worker lifecycle.

The remaining patch is packaging-only. Rebase it on a runtime update and remove
it when upstream supplies these CMake targets with all static dependencies.
