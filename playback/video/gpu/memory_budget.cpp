#include "playback/video/gpu/memory_budget.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <string>

namespace playback_video_gpu {
namespace {

// The four public NVML C ABI entry points used here are dynamically bound to
// the installed NVIDIA driver. No CUDA toolkit or redistributed driver DLL is
// required. Layout/API: docs.nvidia.com/deploy/nvml-api/structnvmlMemory__t.html
// Vulkan's WDDM heap budget is process-local and cannot replace this query.
struct NvmlMemory {
  unsigned long long total;
  unsigned long long free;
  unsigned long long used;
};
static_assert(sizeof(NvmlMemory) == 24);

class Nvml final {
 public:
  Nvml() {
    library_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library_) return;
    const auto initialize = reinterpret_cast<int (*)()>(GetProcAddress(library_, "nvmlInit_v2"));
    shutdown_ = reinterpret_cast<int (*)()>(GetProcAddress(library_, "nvmlShutdown"));
    find_ = reinterpret_cast<Find>(GetProcAddress(library_, "nvmlDeviceGetHandleByPciBusId_v2"));
    memory_ = reinterpret_cast<Memory>(GetProcAddress(library_, "nvmlDeviceGetMemoryInfo"));
    initialized_ = initialize && shutdown_ && find_ && memory_ && initialize() == 0;
  }
  ~Nvml() {
    if (initialized_) shutdown_();
    if (library_) FreeLibrary(library_);
  }
  std::optional<MemoryBudget> query(std::string_view pciBusId) const {
    if (!initialized_ || pciBusId.empty()) return std::nullopt;
    const std::string id(pciBusId);
    void* device = nullptr;
    NvmlMemory memory{};
    if (find_(id.c_str(), &device) != 0 || !device || memory_(device, &memory) != 0 ||
        memory.total == 0 || memory.free > memory.total) return std::nullopt;
    return MemoryBudget{memory.total, memory.free};
  }
 private:
  using Find = int (*)(const char*, void**);
  using Memory = int (*)(void*, NvmlMemory*);
  HMODULE library_ = nullptr;
  int (*shutdown_)() = nullptr;
  Find find_ = nullptr;
  Memory memory_ = nullptr;
  bool initialized_ = false;
};

}  // namespace

std::optional<MemoryBudget> queryMemoryBudget(std::string_view pciBusId) {
  static const Nvml nvml;
  return nvml.query(pciBusId);
}

}  // namespace playback_video_gpu
