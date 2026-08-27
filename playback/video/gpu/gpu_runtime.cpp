#include "gpu_runtime.h"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>

#include "asciiart_gpu.h"

struct GpuRuntime::Impl {
  Impl() : renderer(gpuMutex) {}

  std::recursive_mutex gpuMutex;
  std::mutex initializationMutex;
  GpuAsciiRenderer renderer;
  std::atomic<bool> initialized{false};
};

GpuRuntime::GpuRuntime() : impl_(std::make_unique<Impl>()) {}

GpuRuntime::~GpuRuntime() = default;

GpuAsciiRenderer& GpuRuntime::asciiRenderer() { return impl_->renderer; }

ID3D11Device* GpuRuntime::device() {
  if (!impl_->initialized.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(impl_->initializationMutex);
    if (!impl_->initialized.load(std::memory_order_relaxed)) {
      std::string error;
      if (impl_->renderer.Initialize(&error)) {
        impl_->initialized.store(true, std::memory_order_release);
      }
    }
  }
  return impl_->renderer.device();
}

std::recursive_mutex& GpuRuntime::mutex() { return impl_->gpuMutex; }

void GpuRuntime::resetSessionState() { impl_->renderer.ResetSessionState(); }
