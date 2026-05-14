#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

bool initM4aBackend(const std::filesystem::path& file,
                    uint64_t startFrame,
                    int trackIndex,
                    std::string* error);
void uninitM4aBackend();
bool readM4aBackend(float* out, uint32_t frameCount, uint64_t* framesRead);
bool totalM4aBackend(uint64_t* outFrames);
