#pragma once

#include <cstdint>
#include <vector>

#include "audioplayback.h"

struct AudioPlaybackState;

void audioToggle50Hz(AudioPlaybackState& audio);
bool audioIs50HzEnabled(const AudioPlaybackState& audio);
bool audioSupports50HzToggle(const AudioPlaybackState& audio);

KssPlaybackOptions audioGetKssOptionState(const AudioPlaybackState& audio);
bool audioAdjustKssOption(AudioPlaybackState& audio, KssOptionId id,
                          int direction);
bool audioGetKssInstrumentRegs(AudioPlaybackState& audio,
                               KssInstrumentDevice device,
                               std::vector<uint8_t>* out);
bool audioSetKssInstrumentPreview(AudioPlaybackState& audio,
                                  KssInstrumentDevice device, int channel);
bool audioGetKssInstrumentAuditionState(const AudioPlaybackState& audio,
                                        KssInstrumentDevice* device,
                                        uint32_t* hash);
bool audioStartKssInstrumentAudition(
    AudioPlaybackState& audio, const KssInstrumentProfile& profile);
bool audioStopKssInstrumentAudition(AudioPlaybackState& audio);

NsfPlaybackOptions audioGetNsfOptionState(const AudioPlaybackState& audio);
bool audioAdjustNsfOption(AudioPlaybackState& audio, NsfOptionId id,
                          int direction);

VgmPlaybackOptions audioGetVgmOptionState(const AudioPlaybackState& audio);
bool audioAdjustVgmOption(AudioPlaybackState& audio, VgmOptionId id,
                          int direction);
bool audioGetVgmDeviceOptions(const AudioPlaybackState& audio,
                              uint32_t deviceId, VgmDeviceOptions* out);
bool audioAdjustVgmDeviceOption(AudioPlaybackState& audio,
                                const VgmDeviceInfo& device,
                                const VgmDeviceOptions& baseline,
                                VgmDeviceOptionId id, int direction);
