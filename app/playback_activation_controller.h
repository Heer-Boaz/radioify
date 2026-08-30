#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <variant>

#include "app/audio_activation.h"
#include "app/media_processing_coordinator.h"
#include "app/playback_control_router.h"
#include "app/playback_queue.h"
#include "app/playback_route.h"
#include "audio/playback_session.h"

namespace application_playback {

class PlaybackActivationController;

// Move-only proof that one video activation suspended the previously owned
// transport session. Destroying an unresolved transaction rolls that
// suspension back; only its originating controller can commit or discard it.
// The originating controller must outlive the transaction.
class VideoActivationTransaction {
 public:
  ~VideoActivationTransaction();

  VideoActivationTransaction(VideoActivationTransaction&& other) noexcept;
  VideoActivationTransaction& operator=(
      VideoActivationTransaction&& other) noexcept;

  VideoActivationTransaction(const VideoActivationTransaction&) = delete;
  VideoActivationTransaction& operator=(
      const VideoActivationTransaction&) = delete;

 private:
  VideoActivationTransaction(
      PlaybackActivationController& owner, std::uint64_t id,
      PlaybackControlSessionId displacedControl,
      std::optional<AudioPlaybackSource> displacedAudio);
  void rollback() noexcept;

  PlaybackActivationController* owner_ = nullptr;
  std::uint64_t id_ = 0;
  PlaybackControlSessionId displacedControl_;
  std::optional<AudioPlaybackSource> displacedAudio_;

  friend class PlaybackActivationController;
};

struct PlaybackActivated {
  playback_route::AudioPictureInPicturePlan audioPictureInPicture =
      playback_route::AudioPictureInPicturePlan::Keep;
};

struct AudioActivationFailed {
  std::filesystem::path file;
  audio_playback::FileStartResult endpointResult =
      audio_playback::FileStartResult::RejectedPreservingPlayback;
};

using AudioActivationResult =
    std::variant<PlaybackActivated, AudioActivationFailed>;

struct AudioFallbackDeclined {};
using AudioFallbackResolution =
    std::variant<AudioFallbackDeclined, PlaybackActivated,
                 AudioActivationFailed>;

// Owns playback activation transactions and their interactive resource lease.
// Endpoint start, queue commit and control-identity publication remain ordered
// on the owner thread; no partially prepared activation escapes through the UI.
class PlaybackActivationController {
 public:
  PlaybackActivationController(
      playback_queue::Queue& queue,
      audio_playback::Session& audioPlayback,
      PlaybackControlRouter& playbackControl,
      media_processing::Coordinator& mediaProcessing);

  PlaybackActivationController(const PlaybackActivationController&) = delete;
  PlaybackActivationController& operator=(
      const PlaybackActivationController&) = delete;

  [[nodiscard]] bool interactivePlaybackReady();
  void releaseInteractivePlayback() noexcept;

  [[nodiscard]] AudioActivationResult activateAudio(
      playback_queue::Queue::PreparedActivation activation);
  [[nodiscard]] std::optional<VideoActivationTransaction>
  beginVideoActivation() noexcept;
  [[nodiscard]] std::optional<PlaybackActivated> activateReadyVideo(
      VideoActivationTransaction transaction,
      playback_queue::Queue::PreparedActivation activation);
  [[nodiscard]] bool discardVideoActivation(
      VideoActivationTransaction transaction) noexcept;
  void retireControlSession() noexcept;

  [[nodiscard]] std::optional<AudioFallbackRequest> deferAudioFallback(
      VideoActivationTransaction transaction,
      playback_queue::Queue::PreparedActivation activation,
      playback_session::Problem reason);
  [[nodiscard]] std::optional<AudioFallbackResolution> resolveAudioFallback(
      AudioFallbackDecisionId decision, bool playAudio);

  [[nodiscard]] bool audioFallbackPending() const noexcept;
  [[nodiscard]] std::optional<AudioFallbackDecisionId> cancelAudioFallback()
      noexcept;

 private:
  struct PendingAudioFallback {
    PendingAudioFallback(
        AudioFallbackDecisionId decision,
        playback_queue::Queue::PreparedActivation activation)
        : decision(decision), activation(std::move(activation)) {}

    AudioFallbackDecisionId decision;
    playback_queue::Queue::PreparedActivation activation;
  };

  [[nodiscard]] AudioFallbackDecisionId nextDecisionId() noexcept;
  [[nodiscard]] std::uint64_t nextVideoActivationId() noexcept;
  [[nodiscard]] bool consumeVideoActivation(
      VideoActivationTransaction& transaction, bool restoreAudio) noexcept;
  void rollbackVideoActivation(
      std::uint64_t id, PlaybackControlSessionId displacedControl,
      const std::optional<AudioPlaybackSource>& displacedAudio) noexcept;
  void restoreDisplacedAudio(
      PlaybackControlSessionId displacedControl,
      const std::optional<AudioPlaybackSource>& displacedAudio) noexcept;
  [[nodiscard]] PlaybackActivated commit(
      playback_queue::Queue::PreparedActivation activation);

  playback_queue::Queue& queue_;
  audio_playback::Session& audioPlayback_;
  PlaybackControlRouter& playbackControl_;
  media_processing::Coordinator& mediaProcessing_;
  std::optional<media_processing::Coordinator::InteractivePlaybackLease>
      interactivePlayback_;
  std::optional<PendingAudioFallback> pendingAudioFallback_;
  std::uint64_t lastDecisionValue_ = 0;
  std::uint64_t lastVideoActivationValue_ = 0;
  std::uint64_t activeVideoActivationValue_ = 0;

  friend class VideoActivationTransaction;
};

}  // namespace application_playback
