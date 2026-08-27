#include "playback/media_action_catalog.h"
#include "playback/media_action_context.h"
#include "playback/media_processing_actions.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <optional>
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

  playback_media_processing::RequestResult requestSubtitles(
      const std::filesystem::path& sourceFile) override {
    subtitleRequested = acceptRequests && sourceFile == "source.mp4";
    return requestResult(subtitleRequested,
                         playback_media_processing::RequestFailure::
                             BackendUnavailable);
  }

  playback_media_processing::RequestResult requestSubtitleCancellation()
      override {
    subtitleCancelled = acceptRequests;
    return requestResult(subtitleCancelled,
                         playback_media_processing::RequestFailure::
                             NotRunning);
  }

  playback_media_processing::RequestResult requestAudioExport(
      const std::filesystem::path& sourceFile) override {
    audioExportRequested = acceptRequests && sourceFile == "source.mp4";
    return requestResult(audioExportRequested,
                         playback_media_processing::RequestFailure::
                             BackendUnavailable);
  }

  playback_media_processing::RequestResult requestTranscriptTextExport(
      const std::filesystem::path& sourceFile) override {
    transcriptExportRequested =
        acceptRequests && sourceFile == "source.mp4";
    return requestResult(transcriptExportRequested,
                         playback_media_processing::RequestFailure::
                             MissingTranscript);
  }

  playback_media_processing::RequestResult requestMediaExportCancellation()
      override {
    exportCancelled = acceptRequests;
    return requestResult(exportCancelled,
                         playback_media_processing::RequestFailure::
                             NotRunning);
  }

  playback_media_processing::RequestResult requestAudioSeparation(
      const std::filesystem::path& sourceFile) override {
    separationRequested = acceptRequests && sourceFile == "source.mp4";
    return requestResult(separationRequested,
                         playback_media_processing::RequestFailure::
                             BackendUnavailable);
  }

  playback_media_processing::RequestResult
  requestAudioSeparationCancellation() override {
    separationCancelled = acceptRequests;
    return requestResult(separationCancelled,
                         playback_media_processing::RequestFailure::
                             NotRunning);
  }

  static playback_media_processing::RequestResult requestResult(
      bool accepted, playback_media_processing::RequestFailure failure) {
    return accepted
               ? playback_media_processing::RequestResult::accepted()
               : playback_media_processing::RequestResult::rejected(failure);
  }

  playback_media_processing::SourceState state;
  bool acceptRequests = true;
  bool subtitleRequested = false;
  bool subtitleCancelled = false;
  bool separationRequested = false;
  bool separationCancelled = false;
  bool audioExportRequested = false;
  bool transcriptExportRequested = false;
  bool exportCancelled = false;
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
  ok &= expect(actions::mediaKindForSource("movie.mp4") ==
                       actions::MediaKind::Video &&
                   actions::mediaKindForSource("album.flac") ==
                       actions::MediaKind::Audio &&
                   actions::mediaKindForSource("notes.txt") ==
                       actions::MediaKind::Unsupported,
               "every surface must share source classification with video "
               "taking precedence for media containers");

  actions::Context video;
  video.mediaKind = actions::MediaKind::Video;
  video.canGenerateSubtitles = true;
  video.canSeparateAudio = true;
  video.canExportAudio = true;
  const std::vector<actions::Item> browserVideo = actions::build(video);
  ok &= expect(browserVideo.size() == 5 &&
                   browserVideo[0].action == actions::Action::Play &&
                   browserVideo[1].action == actions::Action::EditVideo &&
                   browserVideo[2].action ==
                       actions::Action::GenerateSubtitles &&
                   browserVideo[2].label == "Generate subtitles..." &&
                   browserVideo[3].action == actions::Action::ExportAudio &&
                   browserVideo[3].label == "Export audio as FLAC" &&
                   browserVideo[4].action ==
                       actions::Action::SeparateAudio &&
                   browserVideo[4].label == "Separate audio...",
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
  video.canExportTranscriptText = true;
  const std::vector<actions::Item> generatedSubtitleVideo =
      actions::build(video);
  const actions::Item* regenerateSubtitles = findAction(
      generatedSubtitleVideo, actions::Action::GenerateSubtitles);
  ok &= expect(regenerateSubtitles &&
                   regenerateSubtitles->label == "Regenerate subtitles..." &&
                   hasAction(generatedSubtitleVideo,
                             actions::Action::ExportTranscriptText),
               "existing generated subtitles must be reflected in the shared "
               "action");
  video.backgroundTaskRunning = true;
  ok &= expect(!hasAction(actions::build(video),
                          actions::Action::GenerateSubtitles) &&
                   !hasAction(actions::build(video),
                              actions::Action::SeparateAudio) &&
                   !hasAction(actions::build(video),
                              actions::Action::ExportAudio) &&
                   !hasAction(actions::build(video),
                              actions::Action::ExportTranscriptText),
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
  video.transcriptTextExportRunningForSource = true;
  const std::vector<actions::Item> exportingTranscriptVideo =
      actions::build(video);
  const actions::Item* cancelTranscriptExport =
      findAction(exportingTranscriptVideo, actions::Action::CancelMediaExport);
  ok &= expect(cancelTranscriptExport &&
                   cancelTranscriptExport->label ==
                       "Cancel transcript export",
               "a transcript export must expose source-specific cancellation");
  video.transcriptTextExportRunningForSource = false;
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
  audio.canExportAudio = true;
  const std::vector<actions::Item> browserAudio = actions::build(audio);
  ok &= expect(browserAudio.size() == 6 &&
                   hasAction(browserAudio, actions::Action::Play) &&
                   hasAction(browserAudio, actions::Action::BrowseTracks) &&
                   hasAction(browserAudio, actions::Action::SeparateAudio) &&
                   hasAction(browserAudio, actions::Action::ExportAudio) &&
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
  processingService.state.subtitleGenerationAvailable = true;
  processingService.state.subtitleGenerationRunning = true;
  processingService.state.hasGeneratedSubtitles = true;
  processingService.state.audioSeparationAvailable = true;
  processingService.state.audioSeparationRunning = true;
  processingService.state.separatedAudioExists = true;
  processingService.state.audioExportAvailable = true;
  processingService.state.audioExportRunning = true;
  processingService.state.transcriptTextExportAvailable = true;
  playback_media_processing::Actions processingActions(processingService);
  const actions::Context projected =
      processingActions.contextForSource("source.mp4");
  ok &= expect(projected.mediaKind == actions::MediaKind::Video &&
                   projected.backgroundTaskRunning &&
                   projected.canGenerateSubtitles &&
                   projected.subtitleGenerationRunningForSource &&
                   projected.hasGeneratedSubtitles &&
                   projected.canSeparateAudio &&
                   projected.audioSeparationRunningForSource &&
                   projected.hasSeparatedAudio && projected.canExportAudio &&
                   projected.audioExportRunningForSource &&
                   projected.canExportTranscriptText,
               "browser and player must share one processing-state projection");
  const std::optional<playback_media_processing::ActionResult>
      generateSubtitles =
      processingActions.execute(actions::Action::GenerateSubtitles,
                                "source.mp4");
  const std::optional<playback_media_processing::ActionResult> separateAudio =
      processingActions.execute(actions::Action::SeparateAudio,
                                "source.mp4");
  const std::optional<playback_media_processing::ActionResult>
      cancelSubtitles =
      processingActions.execute(actions::Action::CancelSubtitleGeneration,
                                "source.mp4");
  const std::optional<playback_media_processing::ActionResult>
      cancelSeparation =
      processingActions.execute(actions::Action::CancelAudioSeparation,
                                "source.mp4");
  const auto exportAudio = processingActions.execute(
      actions::Action::ExportAudio, "source.mp4");
  const auto exportTranscript = processingActions.execute(
      actions::Action::ExportTranscriptText, "source.mp4");
  const auto cancelExport = processingActions.execute(
      actions::Action::CancelMediaExport, "source.mp4");
  const std::optional<playback_media_processing::ActionResult> surfaceAction =
      processingActions.execute(actions::Action::EditVideo, "source.mp4");
  ok &= expect(
      generateSubtitles && generateSubtitles->accepted &&
          generateSubtitles->feedback ==
              "Generating subtitles" &&
          separateAudio && separateAudio->accepted &&
          separateAudio->feedback == "Separating audio" &&
          cancelSubtitles && cancelSubtitles->accepted &&
          cancelSubtitles->feedback == "Cancelling subtitle generation" &&
          cancelSeparation && cancelSeparation->accepted &&
          cancelSeparation->feedback == "Cancelling audio separation" &&
          exportAudio && exportAudio->accepted &&
          exportAudio->feedback == "Exporting audio" && exportTranscript &&
          exportTranscript->accepted &&
          exportTranscript->feedback == "Exporting transcript" &&
          cancelExport && cancelExport->accepted &&
          cancelExport->feedback == "Cancelling export" &&
          processingService.subtitleRequested &&
          processingService.subtitleCancelled &&
          processingService.separationRequested &&
          processingService.separationCancelled &&
          processingService.audioExportRequested &&
          processingService.transcriptExportRequested &&
          processingService.exportCancelled &&
          !surfaceAction,
      "browser and player must share processing dispatch and feedback");
  FakeMediaProcessingService rejectingService;
  rejectingService.acceptRequests = false;
  playback_media_processing::Actions unavailable(rejectingService);
  const std::optional<playback_media_processing::ActionResult>
      rejectedGeneration =
      unavailable.execute(actions::Action::GenerateSubtitles, "source.mp4");
  const std::optional<playback_media_processing::ActionResult>
      rejectedCancellation =
      unavailable.execute(actions::Action::CancelAudioSeparation,
                          "source.mp4");
  ok &= expect(rejectedGeneration && !rejectedGeneration->accepted &&
                   rejectedGeneration->error &&
                   rejectedGeneration->error->failure ==
                       playback_media_processing::RequestFailure::
                           BackendUnavailable &&
                   rejectedGeneration->feedback ==
                       "Subtitle generation could not start: the required "
                       "processing backend is unavailable. Source: "
                       "\"source.mp4\"." &&
                   rejectedCancellation && !rejectedCancellation->accepted &&
                   rejectedCancellation->error &&
                   rejectedCancellation->error->failure ==
                       playback_media_processing::RequestFailure::NotRunning &&
                   rejectedCancellation->feedback ==
                       "Audio separation could not be cancelled: there is no "
                       "matching task to cancel. Source: \"source.mp4\".",
               "a rejecting application service must preserve and explain "
               "the concrete failure");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
