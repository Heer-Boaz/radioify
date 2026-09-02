# Radioify llama.cpp overlay

This port pins the vcpkg `llama-cpp` 7146 recipe used by Radioify and owns the
integration contracts that the upstream package does not currently expose:

- `llama` and `mtmd` are installed in one native CMake export set. Consumers
  receive the generated `llama::llama` and `llama::mtmd` targets with their
  configuration-specific library locations and transitive dependencies.
- `libmtmd` consumes the compiled `miniaudio::miniaudio` package target instead
  of embedding a second implementation. Radioify and mtmd therefore share one
  version, feature configuration, implementation, and set of process-global
  miniaudio state.

When updating llama.cpp, rebase the package integration patch against the new
pinned source and remove it once upstream exports `mtmd` and consumes miniaudio
as a normal dependency.
