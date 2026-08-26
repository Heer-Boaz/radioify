#pragma once

#include <string>

#include "playback/video/transcript/generation_job.h"

std::string subtitleGenerationStatus(
    const playback_video_transcript::GenerationJobSnapshot& snapshot);
