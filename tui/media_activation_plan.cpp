#include "tui/media_activation_plan.h"

#include <optional>
#include <utility>

#include "audio/media_formats.h"
#include "playback/target.h"

namespace tui_media_activation {
namespace {

std::optional<PlaybackPresentationState> requestedVideoPresentation(
    OpenPresentationDirective directive,
    DefaultVideoPresentation defaultPresentation) {
  switch (directive) {
    case OpenPresentationDirective::TerminalAscii:
      return PlaybackPresentationState::terminalAscii();
    case OpenPresentationDirective::NativeWindowedFramebuffer:
      return PlaybackPresentationState::nativeWindowed();
    case OpenPresentationDirective::InheritActive:
      return std::nullopt;
    case OpenPresentationDirective::UseLaunchDefaults:
      return defaultPresentation == DefaultVideoPresentation::TerminalAscii
                 ? PlaybackPresentationState::terminalAscii()
                 : PlaybackPresentationState::nativeWindowed();
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> firstDirectory(
    const std::vector<std::filesystem::path>& files) {
  for (const std::filesystem::path& file : files) {
    std::error_code error;
    if (std::filesystem::is_directory(file, error) && !error) return file;
  }
  return std::nullopt;
}

}  // namespace

Plan planFiles(playback_route::Route route,
               const std::vector<std::filesystem::path>& files) {
  const std::filesystem::path& targetFile =
      playbackTargetFile(route.target);
  if (!isSupportedImageExt(targetFile)) {
    return QueueFiles{std::move(route), files};
  }

  std::vector<std::filesystem::path> images;
  images.reserve(files.size());
  for (const std::filesystem::path& file : files) {
    if (isSupportedImageExt(file)) images.push_back(file);
  }
  std::optional<image_viewer_sequence::Sequence> sequence =
      image_viewer_sequence::Sequence::create(std::move(images), targetFile);
  if (!sequence) {
    return Failure{"The selected images do not form a viewable sequence."};
  }
  return ShowImages{std::move(route), std::move(*sequence)};
}

Plan planOpenFiles(
    const OpenFilesRequest& request,
    DefaultVideoPresentation defaultVideoPresentation) {
  if (std::optional<std::filesystem::path> directory =
          firstDirectory(request.files)) {
    return OpenDirectory{std::move(*directory)};
  }

  std::optional<playback_route::Route> route =
      playback_route::resolveDroppedTarget(
          request.files, nullptr,
          requestedVideoPresentation(request.presentation,
                                     defaultVideoPresentation));
  if (!route) {
    return Failure{"No supported media item was found in the open request."};
  }
  return planFiles(std::move(*route), request.files);
}

}  // namespace tui_media_activation
