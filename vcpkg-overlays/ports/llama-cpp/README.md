# Radioify llama.cpp overlay

This port pins the vcpkg `llama-cpp` 7146 recipe used by Radioify and owns the
two integration contracts that the upstream package does not currently expose:

- `llama::llama` and `llama::mtmd` are published as CMake package targets, so
  consumers do not search the vcpkg installation layout themselves.
- `libmtmd` embeds miniaudio with `MA_API=static`; its implementation-only
  fallback lock is therefore given internal linkage as well. This prevents
  state and symbols from leaking between libmtmd and Radioify's independent
  miniaudio implementation.

When updating llama.cpp, rebase all three patches against the new pinned source
and remove the private-lock patch once upstream provides equivalent linkage.
