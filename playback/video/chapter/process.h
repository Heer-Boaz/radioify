#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "playback/video/chapter/backend.h"

namespace playback_video_chapters {

struct ProcessResult {
  OperationStatus status = OperationStatus::Failed;
  unsigned long exitCode = 0;
  std::string output;
  std::string detail;
};

ProcessResult runHiddenProcess(const std::filesystem::path& executable,
                               const std::vector<std::wstring>& arguments,
                               const OperationControl& control,
                               bool yieldForPlayback,
                               std::size_t maximumCapturedBytes =
                                   8u * 1024u * 1024u);

}  // namespace playback_video_chapters
