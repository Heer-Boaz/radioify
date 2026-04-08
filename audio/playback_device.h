#pragma once

#include <cstdint>

struct AudioPerfStats;

bool audioPlaybackDeviceEnsureRunning();
bool audioPlaybackDeviceRecreate();
void audioPlaybackDeviceUninit();
uint64_t audioPlaybackDeviceLatencyFrames();
void audioPlaybackDeviceFillPerfStats(AudioPerfStats* stats);
