#include "open_file_requests.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

struct OpenFileRequests::Impl {
  Impl() : wakeEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    if (!wakeEvent) {
      throw std::runtime_error("Failed to create open-file request wake event");
    }
  }

  ~Impl() {
    CloseHandle(wakeEvent);
  }

  void post(std::vector<std::filesystem::path> files) {
    if (files.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(files));
      ready.store(true, std::memory_order_release);
    }
    SetEvent(wakeEvent);
  }

  bool poll(std::vector<std::filesystem::path>& out) {
    if (!ready.load(std::memory_order_acquire)) {
      return false;
    }

    std::lock_guard<std::mutex> lock(mutex);
    if (pending.empty()) {
      ready.store(false, std::memory_order_release);
      ResetEvent(wakeEvent);
      return false;
    }

    out = std::move(pending.front());
    pending.pop_front();
    if (pending.empty()) {
      ready.store(false, std::memory_order_release);
      ResetEvent(wakeEvent);
    }
    return true;
  }

  HANDLE wakeEvent = nullptr;
  std::atomic<bool> ready{false};
  std::mutex mutex;
  std::deque<std::vector<std::filesystem::path>> pending;
};

OpenFileRequests::OpenFileRequests() : impl_(std::make_unique<Impl>()) {}

OpenFileRequests::~OpenFileRequests() = default;

void OpenFileRequests::post(std::filesystem::path file) {
  if (file.empty()) {
    return;
  }
  std::vector<std::filesystem::path> files;
  files.push_back(std::move(file));
  impl_->post(std::move(files));
}

void OpenFileRequests::post(std::vector<std::filesystem::path> files) {
  impl_->post(std::move(files));
}

bool OpenFileRequests::poll(std::vector<std::filesystem::path>& out) {
  return impl_->poll(out);
}

HANDLE OpenFileRequests::waitHandle() const {
  return impl_->wakeEvent;
}
