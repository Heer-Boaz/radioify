#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct AudioPlaybackState;

bool initM4aBackend(AudioPlaybackState& audio,
                    const std::filesystem::path& file, uint64_t startFrame,
                    int trackIndex, std::string* error);
void uninitM4aBackend(AudioPlaybackState& audio);
bool totalM4aBackend(AudioPlaybackState& audio, uint64_t* outFrames);
