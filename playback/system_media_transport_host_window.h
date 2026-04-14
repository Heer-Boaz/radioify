#pragma once

class PlaybackSystemTransportHostWindow {
 public:
  PlaybackSystemTransportHostWindow();
  ~PlaybackSystemTransportHostWindow();

  PlaybackSystemTransportHostWindow(PlaybackSystemTransportHostWindow&&) noexcept;
  PlaybackSystemTransportHostWindow& operator=(
      PlaybackSystemTransportHostWindow&&) noexcept;

  PlaybackSystemTransportHostWindow(
      const PlaybackSystemTransportHostWindow&) = delete;
  PlaybackSystemTransportHostWindow& operator=(
      const PlaybackSystemTransportHostWindow&) = delete;

  bool initialize();
  void* nativeHandle() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};
