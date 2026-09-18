#pragma once

#include "playback/video/analysis/edit_review.h"

namespace playback_video_analysis {

std::string reviewIdentity(const ReviewRequest &request);
std::filesystem::path reviewCachePath(const ReviewRequest &request);
bool loadReviewProgress(const ReviewRequest &request, ReviewProgress *progress);
bool storeReviewProgress(const ReviewRequest &request,
                         const ReviewProgress &progress, std::string *error);

} // namespace playback_video_analysis
