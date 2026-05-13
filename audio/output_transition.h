#ifndef RADIOIFY_AUDIO_OUTPUT_TRANSITION_H
#define RADIOIFY_AUDIO_OUTPUT_TRANSITION_H

#include <atomic>
#include <cstdint>

enum class AudioOutputTransitionPhase : uint8_t {
  Idle,
  FadeOutRequested,
  CommitReady,
  CommitInProgress,
};

struct AudioOutputTransition {
  std::atomic<AudioOutputTransitionPhase> phase{
      AudioOutputTransitionPhase::Idle};
  std::atomic<uint32_t> requestSerial{0};
  std::atomic<uint32_t> commitSerial{0};
  std::atomic<uint32_t> fadeInRequestFrames{0};
  uint32_t fadeInFramesRemaining = 0;
  uint32_t fadeInFramesTotal = 0;
};

uint32_t audioOutputTransitionFrames(uint32_t sampleRate);

void audioOutputTransitionReset(AudioOutputTransition& transition);

void audioOutputTransitionRequestFadeIn(AudioOutputTransition& transition,
                                        uint32_t sampleRate);

void audioOutputTransitionRequestDiscontinuity(AudioOutputTransition& transition,
                                               uint32_t sampleRate);

bool audioOutputTransitionBeginFadeOut(AudioOutputTransition& transition);

bool audioOutputTransitionBeginCommit(AudioOutputTransition& transition);

bool audioOutputTransitionWaitingForCommit(
    const AudioOutputTransition& transition);

bool audioOutputTransitionActive(const AudioOutputTransition& transition);

bool audioOutputTransitionFinishCommit(AudioOutputTransition& transition,
                                       uint32_t sampleRate);

void audioOutputTransitionApplyFadeIn(AudioOutputTransition& transition,
                                      float* samples,
                                      uint32_t frames,
                                      uint32_t channels);

void audioOutputTransitionFadeOutToSilence(float* samples,
                                           uint32_t frames,
                                           uint32_t channels,
                                           uint32_t sampleRate);

void audioOutputTransitionFadeTailToSilence(float* samples,
                                            uint32_t audioFrames,
                                            uint32_t channels,
                                            uint32_t sampleRate);

#endif  // RADIOIFY_AUDIO_OUTPUT_TRANSITION_H
