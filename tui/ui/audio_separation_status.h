#pragma once

#include <string>

#include "audio/separation/job.h"

std::string audioSeparationStatus(
    const audio_separation::JobSnapshot& snapshot);
