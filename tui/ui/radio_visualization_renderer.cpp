#include "radio_visualization_renderer.h"
#include "radio_visualization_scene.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "playback/video/ascii/asciiart_layout.h"
#include "tui/consolescreen.h"
#include "tui/ui/ui_helpers.h"

namespace tui_radio_visualization {

Renderer::Renderer(GpuRuntime& gpu) : frames_(gpu) {}

void Renderer::draw(ConsoleScreen& screen, int top, int height, int width,
                    const Levels* levels,
                    RadioFilterMode requestedMode, bool paused) {
  if (height <= 0 || width <= 0) return;
  const Style title{{236,191,115},{0,0,0}};
  const Style dim{{151,145,128},{0,0,0}};
  screen.writeText(0, top, fitLine(" Inside radio  /  typical 1930s", width), title);
  const Presentation presentation = present(levels, requestedMode, paused);
  const char* status = "";
  switch (presentation.status) {
    case Status::SelectTypical: status = " Press R to select the typical radio."; break;
    case Status::UnsupportedReceiver:
      status = " This view is available for the typical radio. Press R to select it.";
      break;
    case Status::Waiting: status = " Waiting for the audio to reach the receiver..."; break;
    case Status::ResumeToObserve: status = " Resume playback to collect circuit activity."; break;
    case Status::Switching: status = " Switching receivers..."; break;
    case Status::NoCircuitObservation: status = " No receiver circuit activity is available."; break;
    case Status::Playing: status = " Signal activity  |  independent stage scales"; break;
    case Status::Paused: status = " Paused  |  circuit activity held"; break;
  }
  if (height > 1) screen.writeText(0, top+1, fitLine(status, width), dim);

  const int available = height - 3;
  if (available < 2) return;
  const Levels* shown = presentation.showCircuit ? levels : nullptr;
  const double cellW = screen.cellPixelWidth() > 0 ? screen.cellPixelWidth() : 8.0;
  const double cellH = screen.cellPixelHeight() > 0 ? screen.cellPixelHeight() : 16.0;
  AsciiArtLayout layout = fitAsciiArtLayout(kWidth, kHeight,
                                             std::min(width, 160), available,
                                             cellW, cellH);
  layout.width = std::clamp(layout.width, 1, width);
  layout.height = std::clamp(layout.height, 1, available);
  const uint64_t generation = levels ? levels->generation : 0;
  if (const AsciiArt* art = frames_.render(shown, generation,
                                           layout.width, layout.height)) {
    const int left = (width - art->width) / 2;
    for (int y=0; y<art->height; ++y) {
      for (int x=0; x<art->width; ++x) {
        const auto& cell = art->cells[static_cast<size_t>(y)*art->width+x];
        screen.writeChar(left+x, top+2+y, cell.ch,
                         {cell.fg, cell.hasBg ? cell.bg : Color{0,0,0}});
      }
    }
  }
  screen.writeText(0, top+height-1,
      fitLine(" Antenna > converter > IF > detector/audio > output > speaker  |  supply below", width),
      dim);
}

}  // namespace tui_radio_visualization
