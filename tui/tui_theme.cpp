#include "tui_theme.h"

namespace {

Color rgb(unsigned char red, unsigned char green, unsigned char blue) {
  Color color;
  color.r = red;
  color.g = green;
  color.b = blue;
  return color;
}

Style style(Color foreground, Color background) {
  Style result;
  result.fg = foreground;
  result.bg = background;
  return result;
}

} // namespace

PlaybackSession::Appearance TuiTheme::playbackSessionAppearance() const {
  PlaybackSession::Appearance result;
  result.baseStyle = normal;
  result.accentStyle = accent;
  result.dimStyle = dim;
  result.progressEmptyStyle = progressEmpty;
  result.progressFrameStyle = progressFrame;
  result.progressStart = progressStart;
  result.progressEnd = progressEnd;
  return result;
}

AudioPictureInPictureWindow::Styles
TuiTheme::audioPictureInPictureStyles() const {
  AudioPictureInPictureWindow::Styles result;
  result.normal = normal;
  result.accent = accent;
  result.dim = dim;
  result.alert = alert;
  result.actionActive = actionActive;
  result.progressEmpty = progressEmpty;
  result.progressFrame = progressFrame;
  result.progressStart = progressStart;
  result.progressEnd = progressEnd;
  return result;
}

tui_melody_visualization::Styles TuiTheme::pitchMonitorStyles() const {
  tui_melody_visualization::Styles result;
  result.normal = normal;
  result.accent = accent;
  result.dim = dim;
  return result;
}

tui_popup_menu::Styles TuiTheme::popupMenuStyles() const {
  tui_popup_menu::Styles result;
  result.normal = normal;
  result.border = dim;
  result.selected = highlight;
  return result;
}

tui_command_palette::Styles TuiTheme::commandPaletteStyles() const {
  tui_command_palette::Styles result;
  result.normal = normal;
  result.border = dim;
  result.dim = dim;
  result.selected = highlight;
  return result;
}

ProgressFooterStyles TuiTheme::progressFooterStyles() const {
  ProgressFooterStyles result;
  result.normal = normal;
  result.progressEmpty = progressEmpty;
  result.progressFrame = progressFrame;
  result.alert = alert;
  result.accent = accent;
  result.progressStart = progressStart;
  result.progressEnd = progressEnd;
  return result;
}

MediaTaskCardStyles TuiTheme::mediaTaskCardStyles() const {
  MediaTaskCardStyles result;
  result.background = normal;
  result.title = accent;
  result.secondary = dim;
  result.progress = normal;
  return result;
}

TuiTheme radioifyTuiTheme() {
  const Color background = rgb(12, 15, 20);
  TuiTheme theme;
  theme.normal = style(rgb(215, 220, 226), background);
  theme.header = style(rgb(230, 238, 248), rgb(18, 28, 44));
  theme.headerGlow = style(rgb(255, 213, 118), rgb(22, 34, 52));
  theme.headerHot = style(rgb(255, 249, 214), rgb(38, 50, 72));
  theme.searchBar = style(rgb(24, 36, 66), rgb(160, 190, 238));
  theme.searchBarGlow = style(rgb(18, 30, 60), rgb(178, 206, 246));
  theme.searchBarActive = style(rgb(14, 24, 50), rgb(196, 220, 248));
  theme.accent = style(rgb(255, 214, 120), background);
  theme.dim = style(rgb(138, 144, 153), background);
  theme.alert = style(rgb(255, 92, 92), background);
  theme.directory = style(rgb(110, 231, 183), background);
  theme.highlight = style(rgb(15, 20, 28), rgb(230, 238, 248));
  theme.browserHover = style(rgb(215, 220, 226), rgb(35, 43, 54));
  theme.breadcrumbHover = style(rgb(15, 20, 28), rgb(255, 214, 120));
  theme.actionActive = style(rgb(15, 20, 28), rgb(255, 214, 120));
  theme.progressStart = rgb(110, 231, 183);
  theme.progressEnd = rgb(255, 214, 110);
  theme.progressEmpty = style(rgb(32, 38, 46), rgb(32, 38, 46));
  theme.progressFrame = style(rgb(160, 170, 182), background);
  return theme;
}
