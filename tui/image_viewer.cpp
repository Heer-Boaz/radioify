#include "tui/image_viewer.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/runtime_helpers.h"
#include "core/windows_message_pump.h"
#include "playback/input/shortcuts.h"
#include "playback/overlay/overlay.h"
#include "playback/video/ascii/asciiart.h"
#include "tui/consoleinput.h"
#include "tui/consolescreen.h"
#include "tui/ui/ui_helpers.h"

namespace image_viewer {

Exit run(image_viewer_sequence::Sequence sequence, ConsoleInput& input,
         ConsoleScreen& screen, const Style& baseStyle,
         const Style& accentStyle, const Style& dimStyle,
         OpenFileRequests& openFileRequests,
         OpenFilesHandler requestOpenFiles) {
  AsciiArt art;
  std::string error;
  int hoverControlToken = -1;
  bool ok = false;
  std::vector<playback_overlay::OverlayControlSpec> controls;
  playback_overlay::OverlayCellLayout controlLayout;
  playback_overlay::InteractionMap controlInteractions;

  auto makeTitle = [&]() {
    return std::string("Preview: ") +
           toUtf8String(sequence.current().filename());
  };

  auto navigateImage = [&](image_viewer_sequence::Direction direction) {
    return sequence.move(direction);
  };

  auto rebuildOverlay = [&]() {
    const int width = std::max(1, screen.width());
    const std::string title = makeTitle();

    playback_overlay::PlaybackOverlayInputs overlayInputs;
    overlayInputs.windowTitle = title;
    overlayInputs.canPlayPrevious = sequence.canMove(
        image_viewer_sequence::Direction::Previous);
    overlayInputs.canPlayNext =
        sequence.canMove(image_viewer_sequence::Direction::Next);
    overlayInputs.osd.controlsVisible = true;

    const playback_overlay::PlaybackOverlayState overlayState =
        playback_overlay::buildPlaybackOverlayState(overlayInputs);
    playback_overlay::OverlayControlSpecOptions controlOptions;
    controlOptions.includeRadio = false;
    controlOptions.includeAudioTrack = false;
    controlOptions.includeSubtitles = false;
    controlOptions.includePictureInPicture = false;
    const int localHoverToken = hoverControlToken;
    controls = playback_overlay::buildOverlayControlSpecs(
        overlayState, localHoverToken, controlOptions);
    controlLayout = playback_overlay::layoutOverlayControlCells(
        playback_overlay::buildOverlayCellControlInputs(controls,
                                                        localHoverToken),
        width);
    controlInteractions =
        playback_overlay::buildOverlayInteractionMap(controlLayout);
  };

  auto renderFrame = [&]() {
    const int width = std::max(1, screen.width());
    const int height = std::max(1, screen.height());
    const std::string title = makeTitle();
    rebuildOverlay();

    const std::vector<std::string> titleLines = wrapLine(title, width);
    const int titleBottom = std::min(height, static_cast<int>(titleLines.size()));
    const int controlTop = std::max(titleBottom, height - controlLayout.height);
    const int artTop = std::clamp(titleBottom, 0, height);
    const int artBottom = std::clamp(controlTop, artTop, height);
    const int maxHeight = std::max(0, artBottom - artTop);

    error.clear();
    ok = true;
    if (maxHeight > 0) {
      ok = renderAsciiArt(sequence.current(), width, maxHeight, art, &error);
    }

    screen.clear(baseStyle);
    if (ok && maxHeight > 0) {
      const int artWidth = std::min(art.width, width);
      const int artHeight = std::min(art.height, maxHeight);
      const int artX = std::max(0, (width - artWidth) / 2);
      for (int y = 0; y < artHeight; ++y) {
        for (int x = 0; x < artWidth; ++x) {
          const auto& cell =
              art.cells[static_cast<size_t>(y * art.width + x)];
          Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
          screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
        }
      }
    } else if (!error.empty()) {
      screen.writeText(0, artTop, fitLine(error, width), dimStyle);
    }

    for (size_t i = 0; i < titleLines.size(); ++i) {
      const int y = static_cast<int>(i);
      if (y < 0 || y >= height) continue;
      screen.writeText(0, y, fitLine(titleLines[i], width), accentStyle);
    }
    for (const auto& item : controlLayout.controls) {
      const int y = controlTop + item.y;
      if (y < 0 || y >= height || item.x >= width) continue;
      Style style = item.enabled
                        ? (item.active ? accentStyle : dimStyle)
                        : dimStyle;
      if (item.enabled && item.hovered) {
        style = Style{style.bg, style.fg};
      }
      screen.writeText(item.x, y, fitLine(item.text, width - item.x), style);
    }
    screen.draw();
  };

  auto clickOverlayControl = [&](playback_overlay::OverlayControlId control) {
    playback_overlay::OverlayControlActions actions;
    actions.previous = [&]() {
      return navigateImage(image_viewer_sequence::Direction::Previous);
    };
    actions.next = [&]() {
      return navigateImage(image_viewer_sequence::Direction::Next);
    };
    return playback_overlay::dispatchOverlayControl(control, actions);
  };

  screen.updateSize();
  renderFrame();

  InputEvent event{};
  OpenFilesRequest openRequest;
  while (true) {
    while (openFileRequests.poll(openRequest)) {
      if (requestOpenFiles(openRequest)) return Exit::Closed;
    }
    while (input.poll(event)) {
      if (event.type == InputEvent::Type::Resize) {
        screen.updateSize();
        renderFrame();
        continue;
      }
      if (event.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = event.mouse;
        if (mouse.kind == MouseEventKind::Press &&
            (mouse.button == MouseButton::Right ||
             mouse.button == MouseButton::Middle)) {
          return Exit::Closed;
        }

        const int width = std::max(1, screen.width());
        const int height = std::max(1, screen.height());
        const std::string title = makeTitle();
        const int titleBottom =
            std::min(height, static_cast<int>(wrapLine(title, width).size()));
        const int controlTop =
            std::max(titleBottom, height - controlLayout.height);
        const int localControlY = mouse.pos.Y - controlTop;
        const std::optional<playback_overlay::OverlayControlId> hitControl =
            (localControlY >= 0 && localControlY < controlLayout.height)
                ? playback_overlay::overlayControlAt(
                      controlInteractions, mouse.pos.X, localControlY)
                : std::nullopt;
        const int hitControlToken =
            hitControl ? playback_overlay::overlayControlToken(*hitControl)
                       : -1;
        if (mouse.kind == MouseEventKind::Move) {
          if (hoverControlToken != hitControlToken) {
            hoverControlToken = hitControlToken;
            renderFrame();
          }
          continue;
        }

        if (isMouseButtonDown(mouse, MouseButton::Left) &&
            mouse.kind == MouseEventKind::Press) {
          if (hitControl && clickOverlayControl(*hitControl)) {
            renderFrame();
            continue;
          }
        }
        continue;
      }
      if (event.type == InputEvent::Type::FileDrop &&
          isCommittedFileDropEvent(event.fileDrop) &&
          requestOpenFiles({event.fileDrop.files})) {
        return Exit::Closed;
      }
      if (event.type == InputEvent::Type::Key ||
          event.type == InputEvent::Type::Action) {
        if (auto shortcut = resolvePlaybackAction(
                event, kPlaybackShortcutContextGlobal |
                           kPlaybackShortcutContextShared |
                           kPlaybackShortcutContextImageViewer)) {
          switch (*shortcut) {
            case PlaybackAction::Quit:
              return Exit::QuitRequested;
            case PlaybackAction::CloseViewer:
              return Exit::Closed;
            case PlaybackAction::Previous:
              if (navigateImage(image_viewer_sequence::Direction::Previous)) {
                renderFrame();
              }
              continue;
            case PlaybackAction::Next:
              if (navigateImage(image_viewer_sequence::Direction::Next)) {
                renderFrame();
              }
              continue;
            case PlaybackAction::SeekBackward:
            case PlaybackAction::SeekForward:
              continue;
            default:
              break;
          }
        }
      }
    }
    NativeWaitHandle handles[2];
    DWORD handleCount = 0;
    if (NativeWaitHandle inputHandle = input.waitHandle()) {
      handles[handleCount++] = inputHandle;
    }
    if (NativeWaitHandle openFilesHandle = openFileRequests.nativeWaitHandle()) {
      handles[handleCount++] = openFilesHandle;
    }
    waitForHandlesAndPumpThreadWindowMessages(
        handleCount, handleCount > 0 ? handles : nullptr, std::nullopt);
  }
}

}  // namespace image_viewer
