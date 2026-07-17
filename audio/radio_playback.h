#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "core/runtime_defaults.h"
#include "radio.h"
#include "radio_filter_mode.h"
#include "radio_filter_transition.h"

struct AudioPipelineTransition;

struct RadioPlaybackFilterConfig {
  uint32_t sampleRate = 48000;
  uint32_t outputChannels = 1;
  float bandwidthHz = static_cast<float>(kDefaultRadioBandwidthHz);
  float noise = static_cast<float>(kDefaultRadioNoiseAmount);
  RadioFilterMode initialMode = kDefaultRadioFilterMode;
  RadioReceptionConfig reception;
  std::string settingsPath;
  std::string presetName;
};

class RadioPlaybackFilter {
 public:
  void initialize(const RadioPlaybackFilterConfig& config);
  void resetSource(uint32_t outputChannels);
  void requestReset();

  RadioFilterMode mode() const;
  void cycleMode();

  void process(float* samples,
               uint32_t frames,
               uint32_t channels,
               AudioPipelineTransition& pipelineTransition);

 private:
  struct Receiver {
    Radio1938 radio;
    RadioPreviewPipeline preview;
  };

  void initializeReceiver(Receiver& receiver,
                          RadioReceiverProfile profile,
                          const RadioPlaybackFilterConfig& config);
  Receiver& receiverFor(RadioFilterMode mode);
  void resetReceiver(RadioFilterMode mode);

  Receiver typical_;
  Receiver philco_;
  RadioAmIngressConfig ingress_;
  RadioPreviewConfig previewConfig_;
  RadioFilterTransition transition_;
  std::vector<float> dryScratch_;
  std::atomic<RadioFilterMode> requestedMode_{kDefaultRadioFilterMode};
  std::atomic<uint64_t> resetGeneration_{0};
  uint64_t appliedResetGeneration_ = 0;
  uint32_t sampleRate_ = 0;
};
