#include "tui/ui/playback_dialogs.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include "core/windows_app_resources.h"
#include "runtime_helpers.h"

namespace tui_playback_dialogs {
namespace {

std::string lowerCase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

void appendText(tui_dialog::Content &content, std::string text,
                tui_dialog::TextTone tone) {
  if (!text.empty())
    content.text.push_back({std::move(text), tone});
}

tui_dialog::Content errorDialog(std::string title, std::string message,
                                std::string detail,
                                const std::filesystem::path &file = {}) {
  tui_dialog::Content content;
  content.title = std::move(title);
  appendText(content, std::move(message), tui_dialog::TextTone::Error);
  appendText(content, std::move(detail), tui_dialog::TextTone::Normal);
  if (!file.empty()) {
    appendText(content, toUtf8String(file.filename()),
               tui_dialog::TextTone::Secondary);
  }
  content.buttons.push_back({0, "Close"});
  return content;
}

void clear(std::optional<tui_dialog::DialogId> &dialog,
           std::optional<application_playback::AudioFallbackDecisionId>
               &decision) {
  dialog.reset();
  decision.reset();
}

} // namespace

tui_dialog::Content audioPlaybackFailure(const std::filesystem::path &file,
                                         std::string warning) {
  if (warning.empty())
    warning = "Failed to start playback.";

  std::string title = "Playback Error";
  std::string message = std::move(warning);
  std::string detail;
  const std::string extension = lowerCase(toUtf8String(file.extension()));
  if ((extension == ".psf2" || extension == ".minipsf2") &&
      message.find("hebios.bin") != std::string::npos) {
    title = "PSF2 BIOS Required";
    message = "Missing hebios.bin for PSF2 playback.";
    detail = "Set RADIOIFY_PSF_BIOS or place hebios.bin next to the file, next "
             "to radioify.exe, or in " RADIOIFY_APP_NAME "'s launch directory.";
  }
  return errorDialog(std::move(title), std::move(message), std::move(detail),
                     file);
}

tui_dialog::Content pictureInPictureFailure(std::string detail) {
  if (detail.empty()) {
    detail = "The picture-in-picture window did not open.";
  }
  return errorDialog("Picture-in-Picture Error",
                     RADIOIFY_APP_NAME " could not open picture-in-picture.",
                     std::move(detail));
}

tui_dialog::Content
videoPlaybackFailure(const std::filesystem::path &file,
                     const playback_session::Problem &problem) {
  const std::string message =
      problem.message.empty() ? "Video playback failed." : problem.message;
  return errorDialog("Video Playback Error", message, problem.detail, file);
}

tui_dialog::Content
audioFallback(const application_playback::AudioFallbackRequest &request) {
  tui_dialog::Content content;
  content.title = "Play audio only?";
  appendText(content,
             request.reason.message.empty() ? "No video stream was found."
                                            : request.reason.message,
             tui_dialog::TextTone::Normal);
  appendText(content, request.reason.detail, tui_dialog::TextTone::Secondary);
  appendText(content, toUtf8String(request.file.filename()),
             tui_dialog::TextTone::Secondary);
  content.buttons.push_back({kPlayAudioButton, "Play audio", "Play"});
  content.buttons.push_back({kCancelButton, "Cancel", "Cancel"});
  content.initiallySelectedButton = kCancelButton;
  return content;
}

void AudioFallbackSession::opened(tui_dialog::DialogId dialog,
                                  application_playback::AudioFallbackDecisionId
                                      decision) {
  dialog_ = dialog;
  decision_ = decision;
}

std::optional<AudioFallbackResolution>
AudioFallbackSession::handle(const tui_dialog::ButtonActivation &activation) {
  if (!dialog_ || !decision_ || activation.dialog != *dialog_) {
    return std::nullopt;
  }
  const AudioFallbackResolution resolution{*decision_, activation.button ==
                                                           kPlayAudioButton};
  clear(dialog_, decision_);
  return resolution;
}

std::optional<AudioFallbackResolution>
AudioFallbackSession::dismissed(tui_dialog::DialogId dialog) {
  if (!dialog_ || !decision_ || dialog != *dialog_) {
    return std::nullopt;
  }
  const AudioFallbackResolution resolution{*decision_, false};
  clear(dialog_, decision_);
  return resolution;
}

std::optional<tui_dialog::DialogId> AudioFallbackSession::revoke(
    application_playback::AudioFallbackDecisionId decision) {
  if (!dialog_ || !decision_ || decision != *decision_) {
    return std::nullopt;
  }
  const tui_dialog::DialogId dialog = *dialog_;
  clear(dialog_, decision_);
  return dialog;
}

} // namespace tui_playback_dialogs
