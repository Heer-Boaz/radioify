#include "gpu_shared.h"

#include <mutex>
#include <string>

#include "asciiart_gpu.h"

GpuAsciiRenderer& sharedGpuRenderer() {
  static GpuAsciiRenderer renderer;
  return renderer;
}

std::recursive_mutex& getSharedGpuMutex() {
  static std::recursive_mutex mtx;
  return mtx;
}

ID3D11Device* getSharedGpuDevice() {
  static bool initialized = false;
  static std::mutex initMutex;

  GpuAsciiRenderer& renderer = sharedGpuRenderer();

  if (!initialized) {
    std::lock_guard<std::mutex> lock(initMutex);
    if (!initialized) {
      std::string error;
      if (renderer.Initialize(1920, 1080, &error)) {
        initialized = true;
      }
    }
  }

  return renderer.device();
}
