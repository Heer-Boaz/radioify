#include "controls.h"

#include <optional>
#include <string>
#include <utility>

#include "core/runtime_helpers.h"
#include "core/windows_app_resources.h"
#include "core/windows_notification_area_icon.h"

namespace {

enum NotificationAreaCommandId : uint32_t {
  kActivateCommand = 1,
  kTogglePauseCommand = 2,
  kStopCommand = 3,
  kPreviousCommand = 4,
  kNextCommand = 5,
  kQuitCommand = 6,
};

std::string filenameLabel(const std::filesystem::path& file) {
  if (file.empty()) {
    return {};
  }
  return toUtf8String(file.filename());
}

bool playbackPresent(const PlaybackControlState& state) {
  return state.active && !state.file.empty();
}

bool isPlaying(const PlaybackControlState& state) {
  return state.status == PlaybackControlStatus::Playing;
}

std::string tooltipForState(const PlaybackControlState& state) {
  if (!playbackPresent(state)) {
    return RADIOIFY_APP_NAME;
  }
  const std::string name = filenameLabel(state.file);
  return name.empty() ? RADIOIFY_APP_NAME
                      : std::string(RADIOIFY_APP_NAME " - ") + name;
}

WindowsNotificationAreaIcon::MenuItem menuItem(
    uint32_t commandId, std::string label, bool separatorBefore = false,
    bool defaultItem = false) {
  WindowsNotificationAreaIcon::MenuItem item;
  item.commandId = commandId;
  item.label = std::move(label);
  item.enabled = true;
  item.separatorBefore = separatorBefore;
  item.defaultItem = defaultItem;
  return item;
}

WindowsNotificationAreaIcon::State iconStateForPlayback(
    const PlaybackControlState& playbackState) {
  const bool hasPlayback = playbackPresent(playbackState);

  WindowsNotificationAreaIcon::State iconState;
  iconState.tooltip = tooltipForState(playbackState);
  iconState.defaultCommandId = kActivateCommand;

  if (isPlaying(playbackState)) {
    if (hasPlayback && playbackState.canPause) {
      iconState.menuItems.push_back(menuItem(kTogglePauseCommand, "Pause"));
    }
  } else if (hasPlayback && playbackState.canPlay) {
    iconState.menuItems.push_back(menuItem(kTogglePauseCommand, "Play"));
  }

  if (hasPlayback && playbackState.canStop &&
      (playbackState.status == PlaybackControlStatus::Playing ||
       playbackState.status == PlaybackControlStatus::Paused)) {
    iconState.menuItems.push_back(menuItem(kStopCommand, "Stop"));
  }

  if (hasPlayback && playbackState.canPrevious) {
    iconState.menuItems.push_back(menuItem(kPreviousCommand, "Previous"));
  }

  if (hasPlayback && playbackState.canNext) {
    iconState.menuItems.push_back(menuItem(kNextCommand, "Next"));
  }

  const bool hasPlaybackCommands = !iconState.menuItems.empty();
  iconState.menuItems.push_back(
      menuItem(kQuitCommand, "Quit " RADIOIFY_APP_NAME, hasPlaybackCommands));
  return iconState;
}

std::optional<PlaybackNotificationAreaCommand> mapCommand(uint32_t commandId) {
  PlaybackNotificationAreaCommand command;
  switch (commandId) {
    case kActivateCommand:
      command.kind = PlaybackNotificationAreaCommand::Kind::Activate;
      return command;
    case kTogglePauseCommand:
      command.kind = PlaybackNotificationAreaCommand::Kind::Playback;
      command.playbackCommand = PlaybackControlCommand::TogglePause;
      return command;
    case kStopCommand:
      command.kind = PlaybackNotificationAreaCommand::Kind::Playback;
      command.playbackCommand = PlaybackControlCommand::Stop;
      return command;
    case kPreviousCommand:
      command.kind = PlaybackNotificationAreaCommand::Kind::Playback;
      command.playbackCommand = PlaybackControlCommand::Previous;
      return command;
    case kNextCommand:
      command.kind = PlaybackNotificationAreaCommand::Kind::Playback;
      command.playbackCommand = PlaybackControlCommand::Next;
      return command;
    case kQuitCommand:
      command.kind = PlaybackNotificationAreaCommand::Kind::Quit;
      return command;
    default:
      return std::nullopt;
  }
}

}  // namespace

struct PlaybackNotificationAreaControls::Impl {
  WindowsNotificationAreaIcon icon{IDI_RADIOIFY_APP_ICON};
  PlaybackControlState state;
  bool initialized = false;

  bool initialize() {
    if (initialized) {
      return icon.available();
    }
    initialized = true;
    return icon.initialize(iconStateForPlayback(state));
  }

  void clear() {
    state = PlaybackControlState{};
    if (initialized) {
      icon.update(iconStateForPlayback(state));
    }
  }

  void update(const PlaybackControlState& nextState) {
    state = nextState;
    if (initialized) {
      icon.update(iconStateForPlayback(state));
    }
  }

  bool pollCommand(PlaybackNotificationAreaCommand* out) {
    if (!out) {
      return false;
    }
    uint32_t commandId = 0;
    while (icon.pollCommand(&commandId)) {
      if (const auto command = mapCommand(commandId)) {
        *out = *command;
        return true;
      }
    }
    return false;
  }
};

PlaybackNotificationAreaControls::PlaybackNotificationAreaControls()
    : impl_(std::make_unique<Impl>()) {}

PlaybackNotificationAreaControls::~PlaybackNotificationAreaControls() = default;

PlaybackNotificationAreaControls::PlaybackNotificationAreaControls(
    PlaybackNotificationAreaControls&&) noexcept = default;

PlaybackNotificationAreaControls& PlaybackNotificationAreaControls::operator=(
    PlaybackNotificationAreaControls&&) noexcept = default;

bool PlaybackNotificationAreaControls::initialize() {
  return impl_->initialize();
}

bool PlaybackNotificationAreaControls::available() const {
  return impl_->icon.available();
}

void PlaybackNotificationAreaControls::clear() { impl_->clear(); }

void PlaybackNotificationAreaControls::update(
    const PlaybackControlState& state) {
  impl_->update(state);
}

bool PlaybackNotificationAreaControls::pollCommand(
    PlaybackNotificationAreaCommand* out) {
  return impl_->pollCommand(out);
}

NativeWaitHandle PlaybackNotificationAreaControls::nativeWaitHandle() const {
  return impl_->icon.nativeWaitHandle();
}
