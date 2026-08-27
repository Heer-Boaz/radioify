#pragma once

#include <cstdint>

struct AudioPerfStats;
struct AudioPlaybackState;

bool audioPlaybackDeviceEnsureRunning(AudioPlaybackState& audio);
void audioPlaybackDeviceUninit(AudioPlaybackState& audio);
uint64_t audioPlaybackDeviceLatencyFrames(AudioPlaybackState& audio);
void audioPlaybackDeviceFillPerfStats(AudioPlaybackState& audio,
                                      AudioPerfStats* stats);
