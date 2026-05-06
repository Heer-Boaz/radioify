#include "open_file_requests.h"

#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include "waitable_signal.h"

struct OpenFileRequests::Impl {
  void post(std::vector<std::filesystem::path> files) {
    if (files.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(files));
      ready.signal();
    }
  }

  bool poll(std::vector<std::filesystem::path>& out) {
    std::lock_guard<std::mutex> lock(mutex);
    if (pending.empty()) {
      ready.clear();
      return false;
    }

    out = std::move(pending.front());
    pending.pop_front();
    if (pending.empty()) {
      ready.clear();
    }
    return true;
  }

  WaitableSignal ready;
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

NativeWaitHandle OpenFileRequests::nativeWaitHandle() const {
  return impl_->ready.nativeWaitHandle();
}
