#pragma once

#include <memory>
#include <mutex>

struct ID3D11Device;
class GpuAsciiRenderer;

// Process-wide owner of the shared D3D11 device and its immediate-context
// serialization. Surfaces borrow this service explicitly; no renderer or
// decoder discovers GPU state through globals.
class GpuRuntime {
 public:
  GpuRuntime();
  ~GpuRuntime();

  GpuRuntime(const GpuRuntime&) = delete;
  GpuRuntime& operator=(const GpuRuntime&) = delete;

  GpuAsciiRenderer& asciiRenderer();
  ID3D11Device* device();
  std::recursive_mutex& mutex();
  void resetSessionState();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
