# Radioify llama.cpp overlay

This port pins the vcpkg `llama-cpp` 7146 recipe used by Radioify and owns the
integration contracts that the upstream package does not currently expose:

- `llama::llama` and `llama::mtmd` are published as CMake package targets, so
  consumers do not search the vcpkg installation layout themselves.
- `libmtmd` consumes the compiled `miniaudio::miniaudio` package target instead
  of embedding a second implementation. Radioify and mtmd therefore share one
  version, feature configuration, implementation, and set of process-global
  miniaudio state.

When updating llama.cpp, rebase the integration patches against the new pinned
source and remove the external-miniaudio patch once upstream exposes miniaudio
as a normal dependency.
