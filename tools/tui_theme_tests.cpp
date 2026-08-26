#include "tui/tui_theme.h"

#include <cstdlib>
#include <iostream>

namespace {

bool sameColor(const Color &left, const Color &right) {
  return left.r == right.r && left.g == right.g && left.b == right.b;
}

bool sameStyle(const Style &left, const Style &right) {
  return sameColor(left.fg, right.fg) && sameColor(left.bg, right.bg);
}

bool expect(bool condition, const char *message) {
  if (condition) {
    return true;
  }
  std::cerr << "tui_theme_tests: " << message << '\n';
  return false;
}

} // namespace

int main() {
  bool ok = true;
  const TuiTheme theme = radioifyTuiTheme();

  ok &= expect(theme.normal.bg.r == 12 && theme.normal.bg.g == 15 &&
                   theme.normal.bg.b == 20 &&
                   sameStyle(theme.actionActive, theme.breadcrumbHover),
               "the Radioify palette must retain its shared base roles");

  const PlaybackSession::Appearance session = theme.playbackSessionAppearance();
  ok &= expect(sameStyle(session.baseStyle, theme.normal) &&
                   sameStyle(session.accentStyle, theme.accent) &&
                   sameStyle(session.dimStyle, theme.dim) &&
                   sameStyle(session.progressEmptyStyle, theme.progressEmpty) &&
                   sameStyle(session.progressFrameStyle, theme.progressFrame) &&
                   sameColor(session.progressStart, theme.progressStart) &&
                   sameColor(session.progressEnd, theme.progressEnd),
               "playback sessions must receive one complete appearance");

  const AudioPictureInPictureWindow::Styles pictureInPicture =
      theme.audioPictureInPictureStyles();
  ok &=
      expect(sameStyle(pictureInPicture.normal, theme.normal) &&
                 sameStyle(pictureInPicture.alert, theme.alert) &&
                 sameStyle(pictureInPicture.actionActive, theme.actionActive) &&
                 sameColor(pictureInPicture.progressEnd, theme.progressEnd),
             "picture-in-picture roles must be projected by name");

  const ProgressFooterStyles footer = theme.progressFooterStyles();
  ok &= expect(sameStyle(footer.progressEmpty, theme.progressEmpty) &&
                   sameStyle(footer.progressFrame, theme.progressFrame) &&
                   sameStyle(footer.alert, theme.alert) &&
                   sameColor(footer.progressStart, theme.progressStart),
               "progress footer roles must be projected by name");

  const MediaTaskCardStyles taskCard = theme.mediaTaskCardStyles();
  const tui_popup_menu::Styles popup = theme.popupMenuStyles();
  const tui_command_palette::Styles palette = theme.commandPaletteStyles();
  const tui_melody_visualization::Styles pitch = theme.pitchMonitorStyles();
  ok &= expect(sameStyle(taskCard.background, theme.normal) &&
                   sameStyle(taskCard.title, theme.accent) &&
                   sameStyle(taskCard.secondary, theme.dim) &&
                   sameStyle(popup.selected, theme.highlight) &&
                   sameStyle(palette.selected, theme.highlight) &&
                   sameStyle(pitch.accent, theme.accent),
               "overlay components must share the semantic theme roles");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
