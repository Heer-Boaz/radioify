#include "subtitle_loader.h"

#include <utility>

#include "core/latest_request_worker.h"

namespace playback_session {

struct SubtitleLoadService::Impl {
  using Worker = LatestRequestWorker<std::filesystem::path, SubtitleManager>;

  explicit Impl(Operation loadOperation)
      : operation(std::move(loadOperation)),
        worker([this](std::filesystem::path file,
                      const Worker::Cancellation& cancellation) {
          const SubtitleManager::CancellationCheck cancelled =
              [&cancellation]() { return cancellation.requested(); };
          return operation(std::move(file), cancelled);
        }) {}

  RequestId allocateRequestId() {
    RequestId requestId = nextRequestId++;
    if (requestId == 0) requestId = nextRequestId++;
    return requestId;
  }

  Operation operation;
  Worker worker;
  RequestId nextRequestId = 1;
  std::optional<RequestId> activeRequest;
};

SubtitleLoadService::SubtitleLoadService(Operation operation)
    : impl_(std::make_unique<Impl>(std::move(operation))) {}

SubtitleLoadService::~SubtitleLoadService() = default;

std::optional<SubtitleLoadService::RequestId>
SubtitleLoadService::start(std::filesystem::path file) {
  const RequestId requestId = impl_->allocateRequestId();
  if (!impl_->worker.submit(requestId, std::move(file))) {
    return std::nullopt;
  }
  impl_->activeRequest = requestId;
  return requestId;
}

bool SubtitleLoadService::cancel(RequestId requestId) {
  if (!impl_->activeRequest || *impl_->activeRequest != requestId) {
    return false;
  }
  impl_->worker.cancel(impl_->allocateRequestId());
  impl_->activeRequest.reset();
  return true;
}

bool SubtitleLoadService::active(RequestId requestId) const {
  return impl_->activeRequest && *impl_->activeRequest == requestId;
}

std::optional<SubtitleLoadService::Completion>
SubtitleLoadService::poll(RequestId requestId) {
  if (!active(requestId)) return std::nullopt;
  std::optional<Impl::Worker::Completion> completion = impl_->worker.poll();
  if (!completion || completion->generation != requestId) {
    return std::nullopt;
  }
  impl_->activeRequest.reset();
  return Completion{completion->generation, std::move(completion->result)};
}

NativeWaitHandle SubtitleLoadService::waitHandle(RequestId requestId) const {
  return active(requestId) ? impl_->worker.nativeWaitHandle()
                           : NativeWaitHandle{};
}

}  // namespace playback_session
