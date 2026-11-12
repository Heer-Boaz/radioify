#include "radio_visualization_renderer.h"
#include "radio_visualization_scene.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "playback/video/ascii/asciiart_layout.h"
#include "tui/consolescreen.h"
#include "tui/ui/ui_helpers.h"

namespace tui_radio_visualization {
namespace {

const char* statusText(Status status) {
  switch (status) {
    case Status::SelectTypical: return " Press R to select the typical radio.";
    case Status::UnsupportedReceiver:
      return " This view is available for the typical radio. Press R to select it.";
    case Status::Waiting: return " Waiting for the audio to reach the receiver...";
    case Status::ResumeToObserve: return " Resume playback to collect circuit activity.";
    case Status::Switching: return " Switching receivers...";
    case Status::NoCircuitObservation: return " No receiver circuit activity is available.";
    case Status::Playing: return " Signal activity  |  independent stage scales";
    case Status::Paused: return " Paused  |  circuit activity held";
  }
  return "";
}

const char* compactStatusText(Status status) {
  switch (status) {
    case Status::SelectTypical: return " Select Typical radio (R).";
    case Status::UnsupportedReceiver: return " Typical radio only (R).";
    case Status::Waiting: return " Waiting for radio signal.";
    case Status::ResumeToObserve: return " Resume to see activity.";
    case Status::Switching: return " Switching receivers...";
    case Status::NoCircuitObservation: return " No circuit data available.";
    case Status::Playing:
    case Status::Paused: return "";
  }
  return "";
}

}  // namespace

Renderer::Renderer(GpuRuntime& gpu) : frames_(gpu) {}

void Renderer::draw(ConsoleScreen& screen, int top, int height, int width,
                    const Snapshot& snapshot) {
  if (height <= 0 || width <= 0) return;
  const Style title{{236,191,115},{0,0,0}};
  const Style dim{{151,145,128},{0,0,0}};
  screen.writeText(0, top, fitLine(" Inside radio  /  typical 1930s", width), title);
  const Levels* levels = snapshot.levels ? &*snapshot.levels : nullptr;
  const Presentation presentation =
      present(levels, snapshot.requestedMode, snapshot.paused);
  if (height > 1) screen.writeText(0, top+1, fitLine(statusText(presentation.status), width), dim);
  const double cellW = screen.cellPixelWidth() > 0 ? screen.cellPixelWidth() : 8.0;
  const double cellH = screen.cellPixelHeight() > 0 ? screen.cellPixelHeight() : 16.0;
  drawCircuit(screen, top + 2, height - 3, width, cellW, cellH, levels,
              presentation.showCircuit);
  if (height > 2) {
    screen.writeText(0, top+height-1,
        fitLine(" Antenna > converter > IF > detector/audio > output > speaker  |  supply below", width),
        dim);
  }
}

void Renderer::drawCompact(ConsoleScreen& screen, int top, int height, int width,
                           double cellWidth, double cellHeight,
                           const Snapshot& snapshot) {
  if (height <= 0 || width <= 0) return;
  const Levels* levels = snapshot.levels ? &*snapshot.levels : nullptr;
  const Presentation presentation =
      present(levels, snapshot.requestedMode, snapshot.paused);
  if (presentation.status != Status::Playing &&
      presentation.status != Status::Paused) {
    const Style dim{{151,145,128},{0,0,0}};
    screen.writeText(0, top,
                     fitLine(compactStatusText(presentation.status), width), dim);
    ++top;
    --height;
  }
  drawCircuit(screen, top, height, width, cellWidth, cellHeight, levels,
              presentation.showCircuit);
}

void Renderer::drawCircuit(ConsoleScreen& screen, int top, int height, int width,
                           double cellWidth, double cellHeight,
                           const Levels* levels, bool showCircuit) {
  if (height < 2 || width <= 0) return;
  const Levels* shown = showCircuit ? levels : nullptr;
  const double cellW = cellWidth > 0 ? cellWidth : 8.0;
  const double cellH = cellHeight > 0 ? cellHeight : 16.0;
  AsciiArtLayout layout = fitAsciiArtLayout(kWidth, kHeight,
                                             std::min(width, 160), height,
                                             cellW, cellH);
  layout.width = std::clamp(layout.width, 1, width);
  layout.height = std::clamp(layout.height, 1, height);
  const uint64_t generation = levels ? levels->generation : 0;
  if (const AsciiArt* art = frames_.render(shown, generation,
                                           layout.width, layout.height)) {
    const int left = (width - art->width) / 2;
    for (int y=0; y<art->height; ++y) {
      for (int x=0; x<art->width; ++x) {
        const auto& cell = art->cells[static_cast<size_t>(y)*art->width+x];
        screen.writeChar(left+x, top+y, cell.ch,
                         {cell.fg, cell.hasBg ? cell.bg : Color{0,0,0}});
      }
    }
  }
}

}  // namespace tui_radio_visualization
