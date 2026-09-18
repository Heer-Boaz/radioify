#include <algorithm>
#include <iostream>
#include <utility>
#include <variant>
#include <vector>

#include "playback/session/input.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "playback_session_input_tests: " << message << '\n';
  return false;
}

InputEvent keyEvent(WORD key, char character = 0, DWORD control = 0) {
  InputEvent event{};
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  event.key.ch = character;
  event.key.control = control;
  return event;
}

class RecordingSession final : public playback_session_input::SessionPort {
 public:
  bool dispatch(playback_session_input::Command command) override {
    commands.push_back(std::move(command));
    return true;
  }

  playback_session_input::SessionSnapshot snapshot() const override {
    return state;
  }

  playback_overlay::InteractionHit hitTest(
      const playback_session_input::InteractionRequest&) const override {
    return hit;
  }

  bool contains(playback_session_input::CommandAction action) const {
    return std::any_of(
        commands.begin(), commands.end(),
        [&](const playback_session_input::Command& command) {
          const auto* actual =
              std::get_if<playback_session_input::CommandAction>(&command);
          return actual && *actual == action;
        });
  }

  template <typename CommandType>
  bool containsType() const {
    return std::any_of(
        commands.begin(), commands.end(),
        [](const playback_session_input::Command& command) {
          return std::holds_alternative<CommandType>(command);
        });
  }

  void clear() { commands.clear(); }

  playback_session_input::SessionSnapshot state;
  playback_overlay::InteractionHit hit;
  std::vector<playback_session_input::Command> commands;
};

bool requestedOverlayRefresh(const RecordingSession& session) {
  using Action = playback_session_input::CommandAction;
  return session.containsType<playback_session_input::ShowPlaybackControls>() &&
         session.contains(Action::RequestWindowPresent) &&
         session.contains(Action::RequestRedraw);
}

bool containsOverlayRefreshCommand(const RecordingSession& session) {
  using Action = playback_session_input::CommandAction;
  return session.containsType<playback_session_input::ShowPlaybackControls>() ||
         session.contains(Action::RequestWindowPresent) ||
         session.contains(Action::RequestRedraw);
}

}  // namespace

int main() {
  using Action = playback_session_input::CommandAction;
  bool ok = true;
  RecordingSession session;
  playback_session_input::PlaybackSeekGestureState seekState;

  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent('S', 'S', SHIFT_PRESSED));
  ok &= expect(session.contains(Action::CopyCurrentVideoFrame) &&
                   !containsOverlayRefreshCommand(session),
               "copying a frame must dispatch the capture command without "
               "covering the captured frame with playback feedback");

  session.clear();
  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent(VK_SPACE, ' '));
  ok &= expect(session.containsType<playback_session_input::SetPaused>() &&
                   requestedOverlayRefresh(session),
               "a contextual pause toggle must publish transport and "
               "playback-feedback commands as one session workflow");

  session.clear();

  session.clear();
  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent('W', 'w', LEFT_CTRL_PRESSED));
  ok &= expect(session.contains(Action::ToggleWindowPresentation) &&
                   !session.containsType<playback_session_input::SetPaused>(),
               "switching to or from native presentation must remain "
               "independent from transport state");

  session.clear();
  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent(VK_RIGHT, 0, LEFT_CTRL_PRESSED));
  ok &= expect(!session.containsType<playback_session_input::TransportRequest>(),
               "retired chapter keys must not switch playlist items");
  MouseEvent suggestionClick{};
  suggestionClick.button = MouseButton::Left;
  suggestionClick.buttons = MouseButtons::Left;
  suggestionClick.kind = MouseEventKind::Press;
  session.clear();
  session.state.videoEditorActive = true;
  session.state.editSuggestionsOpen = true;
  session.hit.editSuggestions = playback_overlay::InteractionMap::SuggestionPanel{
      {0, 0, 50, 20}, 6, 15, {}};
  session.hit.suggestionId = 42;
  playback_session_input::handlePlaybackMouseEvent(session, seekState, suggestionClick);
  ok &= expect(session.containsType<playback_session_input::FocusEditSuggestion>() &&
                   session.containsType<playback_session_input::ClearTimelinePreview>() &&
                   !session.containsType<playback_session_input::VideoEditRequest>() &&
                   !session.containsType<playback_session_input::SeekTo>() &&
                   !session.containsType<playback_session_input::SetPaused>(),
               "a suggestion row must dismiss timeline hover and focus only, without seeking or making an edit");

  session.clear();
  MouseEvent reviewWheel{};
  reviewWheel.kind = MouseEventKind::VerticalWheel;
  reviewWheel.wheelDelta = -120;
  playback_session_input::handlePlaybackMouseEvent(session, seekState, reviewWheel);
  ok &= expect(session.containsType<playback_session_input::SetEditSuggestionsScroll>() &&
                   !session.containsType<playback_session_input::AdjustVolume>() &&
                   !session.containsType<playback_session_input::SeekBy>(),
               "scrolling suggestions must not change volume or transport");

  session.clear();
  session.hit.suggestionId.reset();
  playback_session_input::handlePlaybackMouseEvent(session, seekState, suggestionClick);
  ok &= expect(!session.contains(Action::ToggleFullscreen) &&
                   !session.containsType<playback_session_input::SeekTo>(),
               "empty panel space must consume video gestures");

  session.clear();
  playback_session_input::handlePlaybackInputEvent(session, seekState, keyEvent(VK_DOWN));
  ok &= expect(session.containsType<playback_session_input::VideoEditRequest>() &&
                   !session.containsType<playback_session_input::AdjustVolume>(),
               "arrow keys must navigate suggestions while review is open");
  session.clear();
  playback_session_input::handlePlaybackInputEvent(session, seekState, keyEvent(VK_ESCAPE));
  ok &= expect(session.contains(Action::NavigateBack),
               "Escape must unwind the visible review");

  session.state.videoEditPrompt = playback_video_edit::Prompt::RestartAnalysis;
  session.clear();
  playback_session_input::handlePlaybackInputEvent(session, seekState, keyEvent(VK_RETURN));
  const auto hasEditCommand = [&](playback_video_edit::Command expected) {
    return std::any_of(session.commands.begin(), session.commands.end(), [&](const auto& command) {
      const auto* request = std::get_if<playback_session_input::VideoEditRequest>(&command);
      return request && request->command == expected;
    });
  };
  ok &= expect(session.contains(Action::NavigateBack) &&
                   !hasEditCommand(playback_video_edit::Command::ConfirmPrompt),
               "Enter must preserve current suggestions at the restart confirmation");
  session.clear();
  playback_session_input::handlePlaybackInputEvent(session, seekState, keyEvent('R', 'r'));
  ok &= expect(hasEditCommand(playback_video_edit::Command::ConfirmPrompt),
               "restart confirmation must have its own explicit keyboard action");
  session.clear();
  playback_session_input::handlePlaybackInputEvent(session, seekState, keyEvent(VK_DELETE));
  ok &= expect(!session.containsType<playback_session_input::VideoEditRequest>(),
               "a restart confirmation must block background editing shortcuts");
  session.state.videoEditPrompt = playback_video_edit::Prompt::None;
  session.state.videoEditorActive = false;
  session.state.editSuggestionsOpen = false;
  session.hit = {};

  session.clear();
  session.state.transport.durationUs = 100'000'000;
  playback_session_input::queueSeekRequest(session, seekState, 12.0);
  session.clear();
  playback_session_input::commitQueuedSeek(session, seekState);
  ok &= expect(session.containsType<playback_session_input::SeekTo>() && !seekState.seekQueued,
               "an editor transport intent must be able to consume older queued seek input first");
  session.clear();
  playback_session_input::commitQueuedSeek(session, seekState);
  ok &= expect(!session.containsType<playback_session_input::SeekTo>(),
               "a consumed seek gesture must not be replayed by subsequent preview play/pause");

  session.clear();
  session.state.pictureInPicture = true;
  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent('P', 'p'));
  ok &= expect(session.contains(Action::TogglePictureInPicture) &&
                   !containsOverlayRefreshCommand(session),
               "the local PiP dismiss gesture must close PiP without "
               "requesting feedback on the disappearing surface");

  session.clear();
  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent('P', 'p', LEFT_CTRL_PRESSED));
  ok &= expect(session.contains(Action::TogglePictureInPicture) &&
                   requestedOverlayRefresh(session),
               "the contextual PiP toggle must retain normal playback "
               "feedback even though it shares the same session command");

  return ok ? 0 : 1;
}
