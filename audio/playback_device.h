#pragma once

#include <cstdint>

struct AudioPerfStats;

bool audioPlaybackDeviceEnsureRunning();
void audioPlaybackDeviceUninit();
uint64_t audioPlaybackDeviceLatencyFrames();
void audioPlaybackDeviceFillPerfStats(AudioPerfStats* stats);
