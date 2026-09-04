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
  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent('W', 'w', LEFT_CTRL_PRESSED));
  ok &= expect(session.contains(Action::ToggleWindowPresentation) &&
                   !session.containsType<playback_session_input::SetPaused>(),
               "switching to or from native presentation must remain "
               "independent from transport state");

  session.clear();
  playback_session_input::handlePlaybackInputEvent(
      session, seekState, keyEvent(VK_RIGHT, 0, LEFT_CTRL_PRESSED));
  ok &= expect(
      session.containsType<
          playback_session_input::ChapterNavigationRequest>() &&
          !session.containsType<playback_session_input::TransportRequest>(),
      "Ctrl+Right must request chapter navigation without falling back to "
      "playlist transport");

  session.clear();
  session.hit.chapterStartUs = 120'000'000;
  MouseEvent chapterClick{};
  chapterClick.button = MouseButton::Left;
  chapterClick.buttons = MouseButtons::Left;
  chapterClick.kind = MouseEventKind::Press;
  playback_session_input::handlePlaybackMouseEvent(session, seekState,
                                                    chapterClick);
  const bool sourceChapterTarget = std::any_of(
      session.commands.begin(), session.commands.end(),
      [](const playback_session_input::Command& command) {
        const auto* seek =
            std::get_if<playback_session_input::SeekToChapter>(&command);
        return seek && seek->timelineStartUs == 120'000'000;
      });
  ok &= expect(sourceChapterTarget &&
                   !session.containsType<playback_session_input::SeekTo>(),
               "chapter-row clicks must preserve source coordinates until "
               "the session owner maps them through the current edit list");
  session.hit = {};

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
