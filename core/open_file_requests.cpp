#include "open_file_requests.h"

#include <deque>
#include <memory>
#include <mutex>
#include <utility>

#include "waitable_signal.h"

struct OpenFileRequests::Impl {
  void post(OpenFilesRequest request) {
    if (request.files.empty()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex);
      pending.push_back(std::move(request));
      ready.signal();
    }
  }

  bool hasPending() const {
    std::lock_guard<std::mutex> lock(mutex);
    return !pending.empty();
  }

  bool poll(OpenFilesRequest& out) {
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
  mutable std::mutex mutex;
  std::deque<OpenFilesRequest> pending;
};

OpenFileRequests::OpenFileRequests() : impl_(std::make_unique<Impl>()) {}

OpenFileRequests::~OpenFileRequests() = default;

void OpenFileRequests::post(OpenFilesRequest request) {
  impl_->post(std::move(request));
}

bool OpenFileRequests::hasPending() const {
  return impl_->hasPending();
}

bool OpenFileRequests::poll(OpenFilesRequest& out) {
  return impl_->poll(out);
}

NativeWaitHandle OpenFileRequests::nativeWaitHandle() const {
  return impl_->ready.nativeWaitHandle();
}
