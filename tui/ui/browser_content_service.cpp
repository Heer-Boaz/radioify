#include "browser_content_service.h"

#include <utility>

#include "audio/audioplayback.h"
#include "browser_content_preparation.h"
#include "core/latest_request_worker.h"
#include "optionsbrowser.h"

namespace {

using Worker =
    LatestRequestWorker<BrowserContentRequest, BrowserPreparationResult>;

std::optional<BrowserPreparationResult> prepareBrowserContent(
    BrowserContentRequest request,
    const Worker::Cancellation& cancellation) {
  browser_content_preparation::Result result =
      browser_content_preparation::prepare(
          request, [&cancellation]() { return cancellation.requested(); });
  if (std::holds_alternative<browser_content_preparation::Cancelled>(result)) {
    return std::nullopt;
  }
  if (auto* error = std::get_if<BrowserPreparationError>(&result)) {
    return BrowserPreparationResult(std::move(*error));
  }
  return BrowserPreparationResult(
      std::move(std::get<PreparedBrowserContent>(result)));
}

}  // namespace

struct BrowserContentService::Impl {
  explicit Impl(Config config)
      : config(config), worker(prepareBrowserContent) {}

  Config config;
  Worker worker;
};

BrowserContentService::BrowserContentService(Config config)
    : impl_(std::make_unique<Impl>(config)) {}

BrowserContentService::~BrowserContentService() = default;

BrowserContentPreparation BrowserContentService::prepare(
    BrowserPreparationId preparationId,
    const BrowserContentRequest& request) {
  BrowserContentRequest workerRequest = request;
  if (request.location.kind() == BrowserLocationKind::OptionsBrowser) {
    workerRequest.optionsRuntime = captureOptionsBrowserRuntimeSnapshot(
        request.location, impl_->config.audioPlayback,
        impl_->config.sampleRate, impl_->config.outputChannels);
  }
  if (!impl_->worker.submit(preparationId, std::move(workerRequest))) {
    return BrowserContentPreparation::failed(BrowserPreparationError{
        BrowserPreparationErrorKind::Internal, request.location,
        "The browser preparation worker is unavailable."});
  }
  return BrowserContentPreparation::pending();
}

void BrowserContentService::cancelThrough(
    BrowserPreparationId generation) {
  impl_->worker.cancel(generation);
}

std::optional<BrowserContentService::Completion>
BrowserContentService::poll() {
  std::optional<Worker::Completion> completion = impl_->worker.poll();
  if (!completion) return std::nullopt;
  return Completion{completion->generation, std::move(completion->result)};
}

NativeWaitHandle BrowserContentService::nativeWaitHandle() const {
  return impl_->worker.nativeWaitHandle();
}
