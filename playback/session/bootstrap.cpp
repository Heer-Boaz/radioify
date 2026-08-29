#include "bootstrap.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "playback/session/bootstrap_input.h"

namespace {

double pulseProgress(std::chrono::steady_clock::time_point initStart,
                     std::chrono::steady_clock::time_point now) {
  constexpr double kPrepPulseSeconds = 1.6;
  double elapsed = std::chrono::duration<double>(now - initStart).count();
  double phase = std::fmod(elapsed, kPrepPulseSeconds);
  return (phase <= (kPrepPulseSeconds * 0.5))
             ? (phase / (kPrepPulseSeconds * 0.5))
             : ((kPrepPulseSeconds - phase) / (kPrepPulseSeconds * 0.5));
}

}  // namespace

struct PlaybackSessionBootstrap::Impl {
  enum class Phase : std::uint8_t {
    Created,
    Opening,
    Closing,
    Resolved,
  };

  enum class RequestedDisposition : std::uint8_t {
    Cancel,
    QuitApplication,
  };

  explicit Impl(Args args)
      : file(args.file),
        enableAudio(args.enableAudio),
        enableAscii(args.enableAscii),
        backend(args.backend),
        nowFunction(args.now) {}

  wake_schedule::TimePoint now() const {
    return nowFunction ? nowFunction() : wake_schedule::Clock::now();
  }

  std::optional<playback_session::Problem> openPlayer() {
    return backend.start(
        playback_session::OpeningConfiguration{file, enableAudio, enableAscii});
  }

  void advancePresentation(wake_schedule::TimePoint now) {
    nextDraw = now + kPrepRedrawInterval;
  }

  playback_session::OpenOutcome initFailureOutcome() {
    std::string initError = backend.initializationError();
    if (initError.rfind("No video stream found", 0) == 0) {
      if (!enableAudio) {
        return playback_session::OpenFailure{{
            "No video stream found.", "Audio playback is disabled."}};
      }
      return playback_session::OpenAudioFallback{{
          "No video stream found.",
          "This file can be played as audio only."}};
    }
    if (initError.empty()) {
      initError = "Failed to open video.";
    }
    return playback_session::OpenFailure{{std::move(initError), {}}};
  }

  playback_session::OpenOutcome requestedOutcome() const {
    assert(requestedDisposition);
    return *requestedDisposition == RequestedDisposition::QuitApplication
               ? playback_session::OpenOutcome(
                     playback_session::OpenQuitApplication{})
               : playback_session::OpenOutcome(
                     playback_session::OpenCancelled{});
  }

  void beginClosing(playback_session::OpenOutcome outcome) {
    if (phase == Phase::Closing || phase == Phase::Resolved) return;
    completionAfterClose.emplace(std::move(outcome));
    phase = Phase::Closing;
    backend.requestClose();
    nextDraw = now();
  }

  std::optional<playback_session::OpenOutcome> finishClosing() {
    if (phase != Phase::Closing || !backend.closeReady()) return std::nullopt;
    if (!backend.finishClose()) return std::nullopt;
    assert(completionAfterClose);
    phase = Phase::Resolved;
    playback_session::OpenOutcome outcome =
        requestedDisposition ? requestedOutcome()
                             : std::move(*completionAfterClose);
    requestedDisposition.reset();
    completionAfterClose.reset();
    return outcome;
  }

  std::optional<playback_session::OpenOutcome> resolveInitialization() {
    if (!backend.initializationSucceeded()) {
      beginClosing(initFailureOutcome());
      return finishClosing();
    }
    if (!backend.finishInitialization()) return std::nullopt;
    phase = Phase::Resolved;
    return playback_session::OpenReady{};
  }

  std::optional<playback_session::OpenOutcome> start() {
    assert(phase == Phase::Created);
    if (phase != Phase::Created) return std::nullopt;
    phase = Phase::Opening;
    initStart = now();
    nextDraw = initStart;
    if (std::optional<playback_session::Problem> failure = openPlayer()) {
      phase = Phase::Resolved;
      return playback_session::OpenFailure{std::move(*failure)};
    }
    if (backend.initializationDone()) return resolveInitialization();
    advancePresentation(initStart);
    return std::nullopt;
  }

  std::optional<playback_session::OpenOutcome> pump() {
    if (phase == Phase::Created || phase == Phase::Resolved) {
      return std::nullopt;
    }
    if (phase == Phase::Opening && requestedDisposition) {
      beginClosing(requestedOutcome());
    }
    if (phase == Phase::Opening && backend.initializationDone()) {
      return resolveInitialization();
    }
    if (phase == Phase::Closing) {
      if (std::optional<playback_session::OpenOutcome> outcome =
              finishClosing()) {
        return outcome;
      }
    }

    const wake_schedule::TimePoint currentTime = now();
    if (currentTime >= nextDraw) {
      advancePresentation(currentTime);
    }
    return std::nullopt;
  }

  void request(RequestedDisposition outcome) {
    if (phase == Phase::Created || phase == Phase::Resolved) return;
    if (phase == Phase::Closing) {
      if (outcome == RequestedDisposition::QuitApplication) {
        requestedDisposition = outcome;
      }
      nextDraw = now();
      return;
    }
    if (!requestedDisposition ||
        outcome == RequestedDisposition::QuitApplication) {
      requestedDisposition = outcome;
    }
    beginClosing(requestedOutcome());
    nextDraw = now();
  }

  bool handleInputEvent(const InputEvent& event) {
    if (phase == Phase::Created || phase == Phase::Resolved) return false;
    if (const auto action = playback_session_bootstrap_input::resolve(event)) {
      using BootstrapAction = playback_session_bootstrap_input::Action;
      request(*action == BootstrapAction::QuitApplication
                  ? RequestedDisposition::QuitApplication
                  : RequestedDisposition::Cancel);
      return true;
    }
    if (event.type == InputEvent::Type::Resize) {
      nextDraw = now();
      return true;
    }
    return false;
  }

  static constexpr auto kPrepRedrawInterval =
      std::chrono::milliseconds(120);

  const std::filesystem::path& file;
  const bool enableAudio;
  const bool enableAscii;
  playback_session::OpeningBackend& backend;
  const PlaybackSessionBootstrap::Now nowFunction;
  wake_schedule::TimePoint initStart;
  wake_schedule::TimePoint nextDraw;
  std::optional<RequestedDisposition> requestedDisposition;
  std::optional<playback_session::OpenOutcome> completionAfterClose;
  Phase phase = Phase::Created;
};

PlaybackSessionBootstrap::PlaybackSessionBootstrap(Args args)
    : impl_(std::make_unique<Impl>(std::move(args))) {}

PlaybackSessionBootstrap::~PlaybackSessionBootstrap() = default;

PlaybackSessionBootstrap::PlaybackSessionBootstrap(
    PlaybackSessionBootstrap&&) noexcept = default;

PlaybackSessionBootstrap& PlaybackSessionBootstrap::operator=(
    PlaybackSessionBootstrap&&) noexcept = default;

std::optional<playback_session::OpenOutcome>
PlaybackSessionBootstrap::start() {
  return impl_->start();
}

std::optional<playback_session::OpenOutcome>
PlaybackSessionBootstrap::pump() {
  return impl_->pump();
}

bool PlaybackSessionBootstrap::handleInputEvent(const InputEvent& event) {
  return impl_->handleInputEvent(event);
}

playback_session::TransitionSnapshot PlaybackSessionBootstrap::snapshot()
    const {
  playback_session::TransitionStage stage =
      playback_session::TransitionStage::Opening;
  if (impl_->phase == Impl::Phase::Closing) {
    stage = impl_->requestedDisposition == Impl::RequestedDisposition::Cancel
                ? playback_session::TransitionStage::Cancelling
                : playback_session::TransitionStage::Closing;
  }
  return playback_session::TransitionSnapshot{
      impl_->file, stage,
      pulseProgress(impl_->initStart, impl_->now())};
}

std::vector<NativeWaitHandle> PlaybackSessionBootstrap::waitHandles() const {
  return impl_->phase != Impl::Phase::Created &&
                 impl_->phase != Impl::Phase::Resolved
             ? impl_->backend.waitHandles()
             : std::vector<NativeWaitHandle>{};
}

wake_schedule::Deadline PlaybackSessionBootstrap::nextWakeDeadline() const {
  return impl_->phase != Impl::Phase::Created &&
                 impl_->phase != Impl::Phase::Resolved
             ? wake_schedule::Deadline(impl_->nextDraw)
             : std::nullopt;
}

void PlaybackSessionBootstrap::requestCancel() {
  impl_->request(Impl::RequestedDisposition::Cancel);
}

void PlaybackSessionBootstrap::requestQuit() {
  impl_->request(Impl::RequestedDisposition::QuitApplication);
}
