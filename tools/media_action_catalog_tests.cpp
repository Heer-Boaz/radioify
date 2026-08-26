#include "playback/media_action_catalog.h"
#include "playback/media_processing_actions.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_action_catalog_tests: " << message << '\n';
  return false;
}

bool hasAction(const std::vector<playback_media_actions::Item>& items,
               playback_media_actions::Action action) {
  return std::any_of(items.begin(), items.end(),
                     [action](const auto& item) {
                       return item.action == action;
                     });
}

const playback_media_actions::Item* findAction(
    const std::vector<playback_media_actions::Item>& items,
    playback_media_actions::Action action) {
  const auto found = std::find_if(items.begin(), items.end(),
                                  [action](const auto& item) {
                                    return item.action == action;
                                  });
  return found == items.end() ? nullptr : &*found;
}

class FakeMediaProcessingService final
    : public playback_media_processing::Service {
 public:
  playback_media_processing::SourceState sourceStateFor(
      const std::filesystem::path& sourceFile) const override {
    return sourceFile == "source.mp4"
               ? state
               : playback_media_processing::SourceState{};
  }

  bool requestSubtitles(
      const std::filesystem::path& sourceFile) override {
    subtitleRequested = sourceFile == "source.mp4";
    return subtitleRequested;
  }

  bool requestSubtitleCancellation() override {
    subtitleCancelled = true;
    return true;
  }

  bool requestAudioSeparation(
      const std::filesystem::path& sourceFile) override {
    separationRequested = sourceFile == "source.mp4";
    return separationRequested;
  }

  bool requestAudioSeparationCancellation() override {
    separationCancelled = true;
    return true;
  }

  playback_media_processing::SourceState state;
  bool subtitleRequested = false;
  bool subtitleCancelled = false;
  bool separationRequested = false;
  bool separationCancelled = false;
};

}  // namespace

int main() {
  namespace actions = playback_media_actions;
  bool ok = true;

  actions::Context unsupported;
  unsupported.canBrowseTracks = true;
  unsupported.canAnalyzeAudio = true;
  ok &= expect(actions::build(unsupported).empty(),
               "capabilities must not bypass the media-kind discriminator");

  actions::Context video;
  video.mediaKind = actions::MediaKind::Video;
  video.canSeparateAudio = true;
  const std::vector<actions::Item> browserVideo = actions::build(video);
  ok &= expect(browserVideo.size() == 4 &&
                   browserVideo[0].action == actions::Action::Play &&
                   browserVideo[1].action == actions::Action::EditVideo &&
                   browserVideo[2].action ==
                       actions::Action::GenerateSubtitles &&
                   browserVideo[2].label == "Generate subtitles..." &&
                   browserVideo[3].action ==
                       actions::Action::SeparateAudio &&
                   browserVideo[3].label == "Separate audio...",
               "an idle browser video must expose the canonical source "
               "actions in stable order");

  actions::Context playerVideo = video;
  playerVideo.currentPlayback = true;
  const std::vector<actions::Item> activeVideo = actions::build(playerVideo);
  ok &= expect(activeVideo.size() + 1 == browserVideo.size() &&
                   std::equal(browserVideo.begin() + 1, browserVideo.end(),
                              activeVideo.begin(),
                              [](const auto& browserItem,
                                 const auto& playerItem) {
                                return browserItem.action == playerItem.action &&
                                       browserItem.label == playerItem.label;
                              }),
               "browser and player video menus must share every applicable "
               "source action");

  playerVideo.editorActive = true;
  ok &= expect(!hasAction(actions::build(playerVideo),
                          actions::Action::EditVideo),
               "an active editor must not offer a second edit activation");
  video.hasEdits = true;
  const std::vector<actions::Item> resumableVideo = actions::build(video);
  ok &= expect(resumableVideo.size() >= 2 &&
                   resumableVideo[1].label == "Resume editing",
               "retained edits must change the shared edit label");
  video.hasGeneratedSubtitles = true;
  const std::vector<actions::Item> generatedSubtitleVideo =
      actions::build(video);
  const actions::Item* regenerateSubtitles = findAction(
      generatedSubtitleVideo, actions::Action::GenerateSubtitles);
  ok &= expect(regenerateSubtitles &&
                   regenerateSubtitles->label == "Regenerate subtitles...",
               "existing generated subtitles must be reflected in the shared "
               "action");
  video.backgroundTaskRunning = true;
  ok &= expect(!hasAction(actions::build(video),
                          actions::Action::GenerateSubtitles) &&
                   !hasAction(actions::build(video),
                              actions::Action::SeparateAudio),
               "a running media task must suppress duplicate generation "
               "and separation");
  video.subtitleGenerationRunningForSource = true;
  const std::vector<actions::Item> generatingSubtitleVideo =
      actions::build(video);
  ok &= expect(hasAction(generatingSubtitleVideo,
                         actions::Action::CancelSubtitleGeneration) &&
                   !hasAction(generatingSubtitleVideo,
                              actions::Action::GenerateSubtitles),
               "the source being processed must expose cancellation");
  video.subtitleGenerationRunningForSource = false;
  video.audioSeparationRunningForSource = true;
  const std::vector<actions::Item> separatingVideo = actions::build(video);
  ok &= expect(hasAction(separatingVideo,
                         actions::Action::CancelAudioSeparation) &&
                   !hasAction(separatingVideo,
                              actions::Action::SeparateAudio),
               "a video being separated must expose cancellation");
  video.audioSeparationRunningForSource = false;
  video.backgroundTaskRunning = false;
  video.hasSeparatedAudio = true;
  const std::vector<actions::Item> separatedVideo = actions::build(video);
  const actions::Item* separateAgain =
      findAction(separatedVideo, actions::Action::SeparateAudio);
  ok &= expect(separateAgain &&
                   separateAgain->label == "Separate audio again...",
               "an existing managed stem set must expose explicit replacement");

  actions::Context audio;
  audio.mediaKind = actions::MediaKind::Audio;
  audio.canBrowseTracks = true;
  audio.canAnalyzeAudio = true;
  audio.canSeparateAudio = true;
  const std::vector<actions::Item> browserAudio = actions::build(audio);
  ok &= expect(browserAudio.size() == 5 &&
                   hasAction(browserAudio, actions::Action::Play) &&
                   hasAction(browserAudio, actions::Action::BrowseTracks) &&
                   hasAction(browserAudio, actions::Action::SeparateAudio) &&
                   hasAction(browserAudio, actions::Action::AnalyzeAudio) &&
                   hasAction(browserAudio, actions::Action::SplitLoop) &&
                   !hasAction(browserAudio, actions::Action::GenerateSubtitles) &&
                   !hasAction(browserAudio,
                              actions::Action::CancelSubtitleGeneration),
               "audio menus must expose only canonical audio actions");
  audio.currentPlayback = true;
  audio.backgroundTaskRunning = true;
  const std::vector<actions::Item> busyAudio = actions::build(audio);
  ok &= expect(busyAudio.size() == 1 &&
                   busyAudio.front().action == actions::Action::BrowseTracks,
               "playback and background-task state must project independently");
  audio.audioSeparationRunningForSource = true;
  const std::vector<actions::Item> separatingAudio = actions::build(audio);
  ok &= expect(separatingAudio.size() == 2 &&
                   hasAction(separatingAudio,
                             actions::Action::BrowseTracks) &&
                   hasAction(separatingAudio,
                             actions::Action::CancelAudioSeparation),
               "an audio source being separated must expose cancellation");

  FakeMediaProcessingService processingService;
  processingService.state.backgroundTaskRunning = true;
  processingService.state.subtitleGenerationRunning = true;
  processingService.state.audioSeparationAvailable = true;
  processingService.state.audioSeparationRunning = true;
  processingService.state.separatedAudioExists = true;
  playback_media_processing::Actions processingActions(processingService);
  actions::Context projected;
  processingActions.applySourceState("source.mp4", &projected);
  ok &= expect(projected.backgroundTaskRunning &&
                   projected.subtitleGenerationRunningForSource &&
                   projected.canSeparateAudio &&
                   projected.audioSeparationRunningForSource &&
                   projected.hasSeparatedAudio,
               "browser and player must share one processing-state projection");
  const playback_media_processing::ActionExecution generateSubtitles =
      processingActions.execute(actions::Action::GenerateSubtitles,
                                "source.mp4");
  const playback_media_processing::ActionExecution separateAudio =
      processingActions.execute(actions::Action::SeparateAudio,
                                "source.mp4");
  const playback_media_processing::ActionExecution cancelSubtitles =
      processingActions.execute(actions::Action::CancelSubtitleGeneration,
                                "source.mp4");
  const playback_media_processing::ActionExecution cancelSeparation =
      processingActions.execute(actions::Action::CancelAudioSeparation,
                                "source.mp4");
  const playback_media_processing::ActionExecution surfaceAction =
      processingActions.execute(actions::Action::EditVideo, "source.mp4");
  ok &= expect(
      generateSubtitles.recognized && generateSubtitles.accepted &&
          generateSubtitles.feedback ==
              "Generating subtitles (F8 to cancel)" &&
          separateAudio.recognized && separateAudio.accepted &&
          separateAudio.feedback == "Separating audio (F8 to cancel)" &&
          cancelSubtitles.recognized && cancelSubtitles.accepted &&
          cancelSubtitles.feedback == "Cancelling subtitle generation" &&
          cancelSeparation.recognized && cancelSeparation.accepted &&
          cancelSeparation.feedback == "Cancelling audio separation" &&
          processingService.subtitleRequested &&
          processingService.subtitleCancelled &&
          processingService.separationRequested &&
          processingService.separationCancelled &&
          !surfaceAction.recognized && surfaceAction.feedback.empty(),
      "browser and player must share processing dispatch and feedback");
  playback_media_processing::Actions unavailable;
  const playback_media_processing::ActionExecution rejectedGeneration =
      unavailable.execute(actions::Action::GenerateSubtitles, "source.mp4");
  const playback_media_processing::ActionExecution rejectedCancellation =
      unavailable.execute(actions::Action::CancelAudioSeparation,
                          "source.mp4");
  ok &= expect(rejectedGeneration.recognized &&
                   !rejectedGeneration.accepted &&
                   rejectedGeneration.feedback ==
                       "Could not start subtitle generation" &&
                   rejectedCancellation.recognized &&
                   !rejectedCancellation.accepted &&
                   rejectedCancellation.feedback ==
                       "Could not cancel audio separation",
               "missing application services must fail closed");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
