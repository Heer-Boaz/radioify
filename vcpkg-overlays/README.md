# Radioify vcpkg overlays

These ports are source-controlled extensions of the versions selected by the
`builtin-baseline` in `vcpkg.json`. Keep the upstream vcpkg patches unchanged;
put Radioify-specific behavior in a separately named patch and increment the
port version whenever that behavior changes.

The transcript dependency contract is split at its owning boundaries:

- `ggml/vulkan-static-init.diff` gives Vulkan's dispatcher function-local
  lifetime, so static MSVC builds can use normal GGML backend discovery. It
  addresses the initialization-order failure tracked in
  [whisper.cpp #3750](https://github.com/ggml-org/whisper.cpp/issues/3750).
- `whisper-cpp/require-gpu.diff` adds an opt-in `require_gpu` context parameter,
  so callers can reject GPU initialization failure without parsing log output.
  It also value-initializes Whisper's state so this early failure follows the
  library's normal cleanup path safely.

After changing an overlay, rebuild and install it with:

```powershell
.\build.ps1 -Static -InstallDeps
```
