#include "system_media_transport_controls.h"
#include "system_media_transport_host_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <activation.h>
#include <inspectable.h>
#include <roapi.h>
#include <windows.media.h>
#include <wrl.h>
#include <SystemMediaTransportControlsInterop.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.h>

#include "playback_track_catalog.h"

namespace {

using Microsoft::WRL::ComPtr;
using winrt::Windows::Foundation::TimeSpan;
using winrt::Windows::Media::MediaPlaybackStatus;
using winrt::Windows::Media::MediaPlaybackType;
using winrt::Windows::Media::SystemMediaTransportControls;
using winrt::Windows::Media::SystemMediaTransportControlsButton;
using winrt::Windows::Media::SystemMediaTransportControlsTimelineProperties;

TimeSpan toTimeSpan(double seconds) {
  const double clamped = std::max(0.0, seconds);
  const int64_t ticks = static_cast<int64_t>(std::llround(clamped * 10000000.0));
  return TimeSpan{ticks};
}

std::string utf8FromWide(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                       static_cast<int>(value.size()), nullptr,
                                       0, nullptr, nullptr);
  if (size <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), size, nullptr, nullptr);
  return out;
}

std::string utf8FromPath(const std::filesystem::path& path) {
#ifdef _WIN32
  return utf8FromWide(path.native());
#else
  return path.string();
#endif
}

std::string fallbackMediaTitle(const std::filesystem::path& file) {
  std::string title = utf8FromPath(file.stem());
  if (!title.empty()) {
    return title;
  }
  title = utf8FromPath(file.filename());
  if (!title.empty()) {
    return title;
  }
  return utf8FromPath(file);
}

struct DisplayMetadata {
  std::string title;
  std::string artist;
  uint32_t trackNumber = 0;
};

DisplayMetadata buildDisplayMetadata(const PlaybackSystemControls::State& state) {
  DisplayMetadata metadata;
  metadata.title = fallbackMediaTitle(state.file);
  if (state.trackIndex < 0) {
    return metadata;
  }

  std::vector<TrackEntry> tracks;
  std::string error;
  if (!listPlaybackTracks(normalizePlaybackTrackPath(state.file), &tracks, &error)) {
    metadata.title += " - Track " + std::to_string(state.trackIndex + 1);
    return metadata;
  }

  if (const TrackEntry* track = findPlaybackTrack(tracks, state.trackIndex)) {
    metadata.trackNumber = static_cast<uint32_t>(state.trackIndex + 1);
    if (!track->title.empty()) {
      metadata.artist = metadata.title;
      metadata.title = track->title;
      return metadata;
    }
  }

  metadata.title += " - Track " + std::to_string(state.trackIndex + 1);
  return metadata;
}

MediaPlaybackStatus toPlaybackStatus(PlaybackSystemControls::Status status) {
  switch (status) {
    case PlaybackSystemControls::Status::Playing:
      return MediaPlaybackStatus::Playing;
    case PlaybackSystemControls::Status::Paused:
      return MediaPlaybackStatus::Paused;
    case PlaybackSystemControls::Status::Stopped:
      return MediaPlaybackStatus::Stopped;
    case PlaybackSystemControls::Status::Closed:
    default:
      return MediaPlaybackStatus::Closed;
  }
}

std::optional<PlaybackControlCommand> mapButton(
    SystemMediaTransportControlsButton button) {
  switch (button) {
    case SystemMediaTransportControlsButton::Play:
      return PlaybackControlCommand::Play;
    case SystemMediaTransportControlsButton::Pause:
      return PlaybackControlCommand::Pause;
    case SystemMediaTransportControlsButton::Stop:
      return PlaybackControlCommand::Stop;
    case SystemMediaTransportControlsButton::Previous:
      return PlaybackControlCommand::Previous;
    case SystemMediaTransportControlsButton::Next:
      return PlaybackControlCommand::Next;
    default:
      return std::nullopt;
  }
}

}  // namespace

struct PlaybackSystemControls::Impl {
  bool initialized = false;
  bool available = false;
  bool roInitialized = false;

  PlaybackSystemTransportHostWindow hostWindow;
  SystemMediaTransportControls controls{nullptr};
  SystemMediaTransportControls::ButtonPressed_revoker buttonPressedRevoker;

  State lastState;
  bool haveLastState = false;
  double lastTimelinePositionSec = -1.0;
  double lastTimelineDurationSec = -1.0;

  std::mutex queueMutex;
  std::deque<PlaybackControlCommand> pendingCommands;

  bool initialize() {
    if (initialized) {
      return available;
    }
    initialized = true;

    HRESULT roHr = RoInitialize(RO_INIT_MULTITHREADED);
    if (SUCCEEDED(roHr)) {
      roInitialized = true;
    } else if (roHr != RPC_E_CHANGED_MODE) {
      return false;
    }

    HWND hwnd = nullptr;
    if (hostWindow.initialize()) {
      hwnd = static_cast<HWND>(hostWindow.nativeHandle());
    }
    if (!hwnd) {
      hwnd = GetConsoleWindow();
    }
    if (!hwnd) {
      return false;
    }

    ComPtr<IActivationFactory> factory;
    HSTRING classId = nullptr;
    const wchar_t* runtimeClassName =
        RuntimeClass_Windows_Media_SystemMediaTransportControls;
    HRESULT hr = WindowsCreateString(runtimeClassName,
                                     static_cast<UINT32>(wcslen(runtimeClassName)),
                                     &classId);
    if (FAILED(hr)) {
      return false;
    }

    hr = RoGetActivationFactory(classId, IID_PPV_ARGS(&factory));
    WindowsDeleteString(classId);
    if (FAILED(hr)) {
      return false;
    }

    ComPtr<ISystemMediaTransportControlsInterop> interop;
    hr = factory.As(&interop);
    if (FAILED(hr)) {
      return false;
    }

    ComPtr<ABI::Windows::Media::ISystemMediaTransportControls> rawControls;
    hr = interop->GetForWindow(hwnd, IID_PPV_ARGS(&rawControls));
    if (FAILED(hr)) {
      return false;
    }

    controls = SystemMediaTransportControls(rawControls.Detach(),
                                            winrt::take_ownership_from_abi);
    controls.IsEnabled(false);
    controls.PlaybackStatus(MediaPlaybackStatus::Closed);
    buttonPressedRevoker = controls.ButtonPressed(
        winrt::auto_revoke,
        [this](const SystemMediaTransportControls&,
               const winrt::Windows::Media::
                   SystemMediaTransportControlsButtonPressedEventArgs& args) {
          const auto command = mapButton(args.Button());
          if (!command.has_value()) {
            return;
          }
          std::lock_guard<std::mutex> lock(queueMutex);
          pendingCommands.push_back(*command);
        });
    available = true;
    return true;
  }

  void updateDisplayMetadata(const State& state) {
    DisplayMetadata metadata = buildDisplayMetadata(state);
    auto updater = controls.DisplayUpdater();
    updater.Type(state.isVideo ? MediaPlaybackType::Video
                               : MediaPlaybackType::Music);
    if (state.isVideo) {
      updater.VideoProperties().Title(winrt::to_hstring(metadata.title));
    } else {
      auto music = updater.MusicProperties();
      music.Title(winrt::to_hstring(metadata.title));
      music.Artist(winrt::to_hstring(metadata.artist));
      music.TrackNumber(metadata.trackNumber);
    }
    updater.Update();
  }

  bool timelineChanged(const State& state) const {
    if (state.durationSec <= 0.0) {
      return lastTimelineDurationSec > 0.0;
    }
    if (lastTimelineDurationSec < 0.0) {
      return true;
    }
    if (std::fabs(lastTimelineDurationSec - state.durationSec) >= 0.25) {
      return true;
    }
    return std::fabs(lastTimelinePositionSec - state.positionSec) >= 0.5;
  }

  void updateTimeline(const State& state) {
    if (state.durationSec <= 0.0) {
      lastTimelineDurationSec = -1.0;
      lastTimelinePositionSec = -1.0;
      return;
    }
    SystemMediaTransportControlsTimelineProperties timeline;
    const TimeSpan start = toTimeSpan(0.0);
    const TimeSpan end = toTimeSpan(state.durationSec);
    timeline.StartTime(start);
    timeline.MinSeekTime(start);
    timeline.Position(toTimeSpan(state.positionSec));
    timeline.MaxSeekTime(end);
    timeline.EndTime(end);
    controls.UpdateTimelineProperties(timeline);
    lastTimelineDurationSec = state.durationSec;
    lastTimelinePositionSec = state.positionSec;
  }

  void clear() {
    if (!available) {
      return;
    }
    controls.PlaybackStatus(MediaPlaybackStatus::Closed);
    controls.IsEnabled(false);
    haveLastState = false;
    lastTimelinePositionSec = -1.0;
    lastTimelineDurationSec = -1.0;
  }

  void update(const State& state) {
    if (!available) {
      return;
    }
    if (!state.active || state.file.empty()) {
      clear();
      return;
    }

    controls.IsEnabled(true);

    const bool metadataChanged =
        !haveLastState || lastState.file != state.file ||
        lastState.trackIndex != state.trackIndex ||
        lastState.isVideo != state.isVideo;
    if (metadataChanged) {
      updateDisplayMetadata(state);
    }

    if (!haveLastState || lastState.canPlay != state.canPlay) {
      controls.IsPlayEnabled(state.canPlay);
    }
    if (!haveLastState || lastState.canPause != state.canPause) {
      controls.IsPauseEnabled(state.canPause);
    }
    if (!haveLastState || lastState.canStop != state.canStop) {
      controls.IsStopEnabled(state.canStop);
    }
    if (!haveLastState || lastState.canPrevious != state.canPrevious) {
      controls.IsPreviousEnabled(state.canPrevious);
    }
    if (!haveLastState || lastState.canNext != state.canNext) {
      controls.IsNextEnabled(state.canNext);
    }
    if (!haveLastState || lastState.status != state.status) {
      controls.PlaybackStatus(toPlaybackStatus(state.status));
    }
    if (metadataChanged || timelineChanged(state)) {
      updateTimeline(state);
    }

    lastState = state;
    haveLastState = true;
  }

  bool pollCommand(PlaybackControlCommand* out) {
    if (!out) {
      return false;
    }
    std::lock_guard<std::mutex> lock(queueMutex);
    if (pendingCommands.empty()) {
      return false;
    }
    *out = pendingCommands.front();
    pendingCommands.pop_front();
    return true;
  }

  ~Impl() {
    if (available) {
      clear();
    }
    buttonPressedRevoker = {};
    if (roInitialized) {
      RoUninitialize();
    }
  }
};

PlaybackSystemControls::PlaybackSystemControls()
    : impl_(std::make_unique<Impl>()) {}

PlaybackSystemControls::~PlaybackSystemControls() = default;

PlaybackSystemControls::PlaybackSystemControls(
    PlaybackSystemControls&&) noexcept = default;

PlaybackSystemControls& PlaybackSystemControls::operator=(
    PlaybackSystemControls&&) noexcept = default;

bool PlaybackSystemControls::initialize() { return impl_->initialize(); }

bool PlaybackSystemControls::available() const { return impl_->available; }

void PlaybackSystemControls::clear() { impl_->clear(); }

void PlaybackSystemControls::update(const State& state) { impl_->update(state); }

bool PlaybackSystemControls::pollCommand(PlaybackControlCommand* out) {
  return impl_->pollCommand(out);
}
