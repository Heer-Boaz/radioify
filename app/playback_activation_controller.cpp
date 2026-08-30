#include "app/playback_activation_controller.h"

#include <utility>

#include "playback/target.h"

namespace application_playback {

namespace {

bool sameAudioSource(const AudioPlaybackSource& left,
                     const AudioPlaybackSource& right) {
  return left.file == right.file && left.trackIndex == right.trackIndex;
}

}  // namespace

VideoActivationTransaction::VideoActivationTransaction(
    PlaybackActivationController& owner, std::uint64_t id,
    PlaybackControlSessionId displacedControl,
    std::optional<AudioPlaybackSource> displacedAudio)
    : owner_(&owner),
      id_(id),
      displacedControl_(displacedControl),
      displacedAudio_(std::move(displacedAudio)) {}

VideoActivationTransaction::~VideoActivationTransaction() { rollback(); }

VideoActivationTransaction::VideoActivationTransaction(
    VideoActivationTransaction&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      id_(std::exchange(other.id_, 0)),
      displacedControl_(std::exchange(other.displacedControl_, {})),
      displacedAudio_(std::move(other.displacedAudio_)) {}

VideoActivationTransaction& VideoActivationTransaction::operator=(
    VideoActivationTransaction&& other) noexcept {
  if (this == &other) return *this;
  rollback();
  owner_ = std::exchange(other.owner_, nullptr);
  id_ = std::exchange(other.id_, 0);
  displacedControl_ = std::exchange(other.displacedControl_, {});
  displacedAudio_ = std::move(other.displacedAudio_);
  return *this;
}

void VideoActivationTransaction::rollback() noexcept {
  PlaybackActivationController* owner = std::exchange(owner_, nullptr);
  if (!owner) return;
  owner->rollbackVideoActivation(id_, displacedControl_, displacedAudio_);
  id_ = 0;
  displacedControl_ = {};
  displacedAudio_.reset();
}

PlaybackActivationController::PlaybackActivationController(
    playback_queue::Queue& queue,
    audio_playback::Session& audioPlayback,
    PlaybackControlRouter& playbackControl,
    media_processing::Coordinator& mediaProcessing)
    : queue_(queue),
      audioPlayback_(audioPlayback),
      playbackControl_(playbackControl),
      mediaProcessing_(mediaProcessing) {}

bool PlaybackActivationController::interactivePlaybackReady() {
  if (!interactivePlayback_) {
    interactivePlayback_.emplace(
        mediaProcessing_.acquireInteractivePlayback());
  }
  return interactivePlayback_->ready();
}

void PlaybackActivationController::releaseInteractivePlayback() noexcept {
  interactivePlayback_.reset();
}

AudioActivationResult PlaybackActivationController::activateAudio(
    playback_queue::Queue::PreparedActivation activation) {
  releaseInteractivePlayback();
  const std::filesystem::path file =
      playbackTargetFile(activation.route().target);
  const int trackIndex =
      playbackTargetTrackIndex(activation.route().target).value_or(0);
  const audio_playback::FileStartResult endpointResult =
      audioPlayback_.startFile(file, trackIndex);
  if (!audio_playback::fileStartSucceeded(endpointResult)) {
    if (endpointResult ==
        audio_playback::FileStartResult::FailedAfterReplacingPlayback) {
      playbackControl_.endSession();
    }
    return AudioActivationFailed{file, endpointResult};
  }
  return commit(std::move(activation));
}

std::optional<VideoActivationTransaction>
PlaybackActivationController::beginVideoActivation() noexcept {
  if (activeVideoActivationValue_ != 0) return std::nullopt;

  const PlaybackControlSessionId displacedControl =
      playbackControl_.sessionId();
  const AudioPlaybackSnapshot audio = audioPlayback_.snapshot();
  std::optional<AudioPlaybackSource> displacedAudio;
  if (displacedControl.valid() && audio.ready && audio.source) {
    displacedAudio = audio.source;
  }
  const std::uint64_t id = nextVideoActivationId();
  activeVideoActivationValue_ = id;
  playbackControl_.endSession();
  return VideoActivationTransaction(*this, id, displacedControl,
                                    std::move(displacedAudio));
}

std::optional<PlaybackActivated>
PlaybackActivationController::activateReadyVideo(
    VideoActivationTransaction transaction,
    playback_queue::Queue::PreparedActivation activation) {
  if (!consumeVideoActivation(transaction, false)) return std::nullopt;
  return commit(std::move(activation));
}

bool PlaybackActivationController::discardVideoActivation(
    VideoActivationTransaction transaction) noexcept {
  return consumeVideoActivation(transaction, false);
}

void PlaybackActivationController::retireControlSession() noexcept {
  playbackControl_.endSession();
}

std::optional<AudioFallbackRequest>
PlaybackActivationController::deferAudioFallback(
    VideoActivationTransaction transaction,
    playback_queue::Queue::PreparedActivation activation,
    playback_session::Problem reason) {
  if (pendingAudioFallback_) return std::nullopt;
  if (!consumeVideoActivation(transaction, true)) return std::nullopt;

  const AudioFallbackDecisionId decision = nextDecisionId();
  const std::filesystem::path file =
      playbackTargetFile(activation.route().target);
  pendingAudioFallback_.emplace(decision, std::move(activation));
  return AudioFallbackRequest{decision, file, std::move(reason)};
}

std::optional<AudioFallbackResolution>
PlaybackActivationController::resolveAudioFallback(
    AudioFallbackDecisionId decision, bool playAudio) {
  if (!pendingAudioFallback_ ||
      pendingAudioFallback_->decision != decision) {
    return std::nullopt;
  }

  playback_queue::Queue::PreparedActivation activation =
      std::move(pendingAudioFallback_->activation);
  pendingAudioFallback_.reset();
  releaseInteractivePlayback();
  if (!playAudio) return AudioFallbackDeclined{};

  AudioActivationResult result = activateAudio(std::move(activation));
  if (auto* failure = std::get_if<AudioActivationFailed>(&result)) {
    return std::move(*failure);
  }
  return std::move(std::get<PlaybackActivated>(result));
}

bool PlaybackActivationController::audioFallbackPending() const noexcept {
  return pendingAudioFallback_.has_value();
}

std::optional<AudioFallbackDecisionId>
PlaybackActivationController::cancelAudioFallback() noexcept {
  if (!pendingAudioFallback_) return std::nullopt;
  const AudioFallbackDecisionId decision = pendingAudioFallback_->decision;
  pendingAudioFallback_.reset();
  return decision;
}

AudioFallbackDecisionId
PlaybackActivationController::nextDecisionId() noexcept {
  ++lastDecisionValue_;
  if (lastDecisionValue_ == 0) ++lastDecisionValue_;
  return {lastDecisionValue_};
}

std::uint64_t PlaybackActivationController::nextVideoActivationId()
    noexcept {
  ++lastVideoActivationValue_;
  if (lastVideoActivationValue_ == 0) ++lastVideoActivationValue_;
  return lastVideoActivationValue_;
}

bool PlaybackActivationController::consumeVideoActivation(
    VideoActivationTransaction& transaction, bool restoreAudio) noexcept {
  if (transaction.owner_ != this || transaction.id_ == 0 ||
      transaction.id_ != activeVideoActivationValue_) {
    return false;
  }

  activeVideoActivationValue_ = 0;
  transaction.owner_ = nullptr;
  transaction.id_ = 0;
  if (restoreAudio) {
    restoreDisplacedAudio(transaction.displacedControl_,
                          transaction.displacedAudio_);
  }
  transaction.displacedControl_ = {};
  transaction.displacedAudio_.reset();
  return true;
}

void PlaybackActivationController::rollbackVideoActivation(
    std::uint64_t id, PlaybackControlSessionId displacedControl,
    const std::optional<AudioPlaybackSource>& displacedAudio) noexcept {
  if (id == 0 || id != activeVideoActivationValue_) return;
  activeVideoActivationValue_ = 0;
  restoreDisplacedAudio(displacedControl, displacedAudio);
}

void PlaybackActivationController::restoreDisplacedAudio(
    PlaybackControlSessionId displacedControl,
    const std::optional<AudioPlaybackSource>& displacedAudio) noexcept {
  if (!displacedControl.valid() || !displacedAudio ||
      playbackControl_.sessionId().valid()) {
    return;
  }
  const AudioPlaybackSnapshot current = audioPlayback_.snapshot();
  if (!current.ready || !current.source ||
      !sameAudioSource(*current.source, *displacedAudio)) {
    return;
  }
  playbackControl_.beginSession();
}

PlaybackActivated PlaybackActivationController::commit(
    playback_queue::Queue::PreparedActivation activation) {
  const playback_route::AudioPictureInPicturePlan audioPictureInPicture =
      activation.route().audioPictureInPicture;
  playbackControl_.endSession();
  queue_.commit(std::move(activation));
  playbackControl_.beginSession();
  return PlaybackActivated{audioPictureInPicture};
}

}  // namespace application_playback
