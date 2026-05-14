#ifndef RADIOIFY_AUDIO_PIPELINE_TRANSITION_H
#define RADIOIFY_AUDIO_PIPELINE_TRANSITION_H

#include <atomic>
#include <cstdint>

enum class AudioPipelineTransitionPhase : uint8_t {
  Idle,
  FadeOutRequested,
  CommitReady,
  CommitInProgress,
};

struct AudioPipelineTransition {
  std::atomic<AudioPipelineTransitionPhase> phase{
      AudioPipelineTransitionPhase::Idle};
  std::atomic<uint32_t> requestSerial{0};
  std::atomic<uint32_t> commitSerial{0};
  std::atomic<uint32_t> inputFadeInRequestFrames{0};
  std::atomic<uint32_t> outputFadeInRequestFrames{0};
  uint32_t inputFadeInFramesRemaining = 0;
  uint32_t inputFadeInFramesTotal = 0;
  uint32_t outputFadeInFramesRemaining = 0;
  uint32_t outputFadeInFramesTotal = 0;
};

uint32_t audioPipelineTransitionFrames(uint32_t sampleRate);

void audioPipelineTransitionReset(AudioPipelineTransition& transition);

void audioPipelineTransitionRequestOutputFadeIn(
    AudioPipelineTransition& transition,
    uint32_t sampleRate);

void audioPipelineTransitionRequestSignalFadeIn(
    AudioPipelineTransition& transition,
    uint32_t sampleRate);

void audioPipelineTransitionRequestDiscontinuity(
    AudioPipelineTransition& transition,
    uint32_t sampleRate);

bool audioPipelineTransitionBeginFadeOut(AudioPipelineTransition& transition);

bool audioPipelineTransitionBeginCommit(AudioPipelineTransition& transition);

bool audioPipelineTransitionWaitingForCommit(
    const AudioPipelineTransition& transition);

bool audioPipelineTransitionActive(const AudioPipelineTransition& transition);

bool audioPipelineTransitionFinishCommit(AudioPipelineTransition& transition,
                                         uint32_t sampleRate);

void audioPipelineTransitionApplyInputFadeIn(
    AudioPipelineTransition& transition,
    float* samples,
    uint32_t frames,
    uint32_t channels);

void audioPipelineTransitionApplyFadeIn(AudioPipelineTransition& transition,
                                        float* samples,
                                        uint32_t frames,
                                        uint32_t channels);

void audioPipelineTransitionFadeOutToSilence(float* samples,
                                             uint32_t frames,
                                             uint32_t channels,
                                             uint32_t sampleRate);

void audioPipelineTransitionFadeTailToSilence(float* samples,
                                              uint32_t audioFrames,
                                              uint32_t channels,
                                              uint32_t sampleRate);

#endif  // RADIOIFY_AUDIO_PIPELINE_TRANSITION_H
