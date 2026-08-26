#include "tui/media_activation_plan.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "playback/target.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_activation_plan_tests: " << message << '\n';
  return false;
}

playback_route::Route routeFor(const std::filesystem::path& file) {
  playback_route::Route route;
  route.target = playbackFileTarget(file);
  return route;
}

}  // namespace

int main() {
  namespace activation = tui_media_activation;
  bool ok = true;

  const std::vector<std::filesystem::path> videoFiles = {
      "first.mp4", "second.mp4"};
  activation::Plan video =
      activation::planFiles(routeFor("first.mp4"), videoFiles);
  const auto* queuedVideo = std::get_if<activation::QueueFiles>(&video);
  ok &= expect(queuedVideo && queuedVideo->files == videoFiles &&
                   playbackTargetFile(queuedVideo->route.target) ==
                       std::filesystem::path("first.mp4"),
               "video files must retain their queue source and route");

  const std::vector<std::filesystem::path> mixedImages = {
      "first.jpg", "notes.txt", "second.png"};
  activation::Plan images =
      activation::planFiles(routeFor("second.png"), mixedImages);
  const auto* imageViewer = std::get_if<activation::ShowImages>(&images);
  ok &= expect(imageViewer &&
                   imageViewer->sequence.current() ==
                       std::filesystem::path("second.png") &&
                   imageViewer->sequence.canMove(
                       image_viewer_sequence::Direction::Previous) &&
                   !imageViewer->sequence.canMove(
                       image_viewer_sequence::Direction::Next),
               "image activation must filter non-images and preserve focus");

  OpenFilesRequest terminalRequest;
  terminalRequest.files = {"movie.mp4"};
  terminalRequest.presentation = OpenPresentationDirective::UseLaunchDefaults;
  activation::Plan terminal = activation::planOpenFiles(
      terminalRequest, activation::DefaultVideoPresentation::TerminalAscii);
  const auto* terminalVideo = std::get_if<activation::QueueFiles>(&terminal);
  ok &= expect(terminalVideo && terminalVideo->route.videoContinuation &&
                   terminalVideo->route.videoContinuation->presentation &&
                   terminalVideo->route.videoContinuation->presentation
                       ->usesAsciiGrid(),
               "launch defaults must resolve to terminal presentation once");

  OpenFilesRequest inheritedRequest = terminalRequest;
  inheritedRequest.presentation = OpenPresentationDirective::InheritActive;
  activation::Plan inherited = activation::planOpenFiles(
      inheritedRequest,
      activation::DefaultVideoPresentation::NativeWindowedFramebuffer);
  const auto* inheritedVideo =
      std::get_if<activation::QueueFiles>(&inherited);
  ok &= expect(inheritedVideo && !inheritedVideo->route.videoContinuation,
               "inherited presentation must not manufacture session state");

  OpenFilesRequest unsupportedRequest;
  unsupportedRequest.files = {"notes.txt"};
  activation::Plan unsupported = activation::planOpenFiles(
      unsupportedRequest,
      activation::DefaultVideoPresentation::NativeWindowedFramebuffer);
  ok &= expect(std::holds_alternative<activation::Failure>(unsupported),
               "unsupported open requests must fail during planning");

  const auto stamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path directory =
      std::filesystem::temp_directory_path() /
      ("radioify-media-activation-" + std::to_string(stamp));
  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error) {
    std::cerr << "media_activation_plan_tests: could not create directory\n";
    return EXIT_FAILURE;
  }
  OpenFilesRequest directoryRequest;
  directoryRequest.files = {directory, "movie.mp4"};
  activation::Plan directoryPlan = activation::planOpenFiles(
      directoryRequest,
      activation::DefaultVideoPresentation::NativeWindowedFramebuffer);
  const auto* openDirectory =
      std::get_if<activation::OpenDirectory>(&directoryPlan);
  ok &= expect(openDirectory && openDirectory->path == directory,
               "directory requests must take precedence over media playback");
  std::filesystem::remove_all(directory, error);

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
