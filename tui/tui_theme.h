#pragma once

#include "audio_picture_in_picture_window.h"
#include "playback/session/session.h"
#include "tui/ui/browser_action_strip_renderer.h"
#include "tui/ui/command_palette_renderer.h"
#include "tui/ui/media_task_card.h"
#include "tui/ui/melody_visualization_renderer.h"
#include "tui/ui/popup_menu_renderer.h"
#include "tui/ui/ui_helpers.h"

struct TuiTheme {
  Style normal;
  Style header;
  Style headerGlow;
  Style headerHot;
  Style searchBar;
  Style searchBarGlow;
  Style searchBarActive;
  Style accent;
  Style dim;
  Style alert;
  Style directory;
  Style highlight;
  Style browserHover;
  Style breadcrumbHover;
  Style actionActive;
  Style progressEmpty;
  Style progressFrame;
  Color progressStart;
  Color progressEnd;

  PlaybackSession::Appearance playbackSessionAppearance() const;
  AudioPictureInPictureWindow::Styles audioPictureInPictureStyles() const;
  tui_melody_visualization::Styles pitchMonitorStyles() const;
  tui_popup_menu::Styles popupMenuStyles() const;
  tui_command_palette::Styles commandPaletteStyles() const;
  browser_action_strip::Styles browserActionStripStyles() const;
  ProgressFooterStyles progressFooterStyles() const;
  MediaTaskCardStyles mediaTaskCardStyles() const;
};

TuiTheme radioifyTuiTheme();
