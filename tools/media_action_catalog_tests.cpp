#include "playback/media_action_catalog.h"

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
  const std::vector<actions::Item> browserVideo = actions::build(video);
  ok &= expect(browserVideo.size() == 3 &&
                   browserVideo[0].action == actions::Action::Play &&
                   browserVideo[1].action == actions::Action::EditVideo &&
                   browserVideo[2].action ==
                       actions::Action::CreateIndexedTranscript &&
                   browserVideo[2].label == "Generate subtitles...",
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
  video.hasIndexedTranscript = true;
  const std::vector<actions::Item> transcriptVideo = actions::build(video);
  ok &= expect(transcriptVideo.back().action ==
                       actions::Action::CreateIndexedTranscript &&
                   transcriptVideo.back().label == "Regenerate subtitles...",
               "an existing transcript must be reflected in the shared action");
  video.backgroundTaskRunning = true;
  ok &= expect(!hasAction(actions::build(video),
                          actions::Action::CreateIndexedTranscript),
               "a running media task must suppress duplicate transcription");
  video.indexedTranscriptRunningForSource = true;
  const std::vector<actions::Item> transcribingVideo = actions::build(video);
  ok &= expect(hasAction(transcribingVideo,
                         actions::Action::CancelIndexedTranscript) &&
                   !hasAction(transcribingVideo,
                              actions::Action::CreateIndexedTranscript),
               "the source being transcribed must expose cancellation");

  actions::Context audio;
  audio.mediaKind = actions::MediaKind::Audio;
  audio.canBrowseTracks = true;
  audio.canAnalyzeAudio = true;
  const std::vector<actions::Item> browserAudio = actions::build(audio);
  ok &= expect(browserAudio.size() == 4 &&
                   hasAction(browserAudio, actions::Action::Play) &&
                   hasAction(browserAudio, actions::Action::BrowseTracks) &&
                   hasAction(browserAudio, actions::Action::AnalyzeAudio) &&
                   hasAction(browserAudio, actions::Action::SplitLoop) &&
                   !hasAction(browserAudio,
                              actions::Action::CreateIndexedTranscript) &&
                   !hasAction(browserAudio,
                              actions::Action::CancelIndexedTranscript),
               "audio menus must expose only canonical audio actions");
  audio.currentPlayback = true;
  audio.backgroundTaskRunning = true;
  const std::vector<actions::Item> busyAudio = actions::build(audio);
  ok &= expect(busyAudio.size() == 1 &&
                   busyAudio.front().action == actions::Action::BrowseTracks,
               "playback and background-task state must project independently");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
