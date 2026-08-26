#include "melody_visualization_renderer.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "core/unicode_display_width.h"
#include "tui/ui/ui_helpers.h"

namespace tui_melody_visualization {

void draw(ConsoleScreen &screen, const Model &model, const Bounds &bounds,
          const Styles &styles) {
  if (!model.active() || bounds.height <= 0 || bounds.width <= 0) {
    return;
  }

  const AudioMelodyInfo &pitch = model.pitch();
  const AudioMelodyAnalysisState &analysis = model.analysis();
  const int bottom = bounds.top + bounds.height;
  int row = bounds.top;

  if (row < bottom) {
    screen.writeText(0, row++, fitLine(" Pitch monitor", bounds.width),
                     styles.accent);
  }

  if (row < bottom) {
    std::string noteLine = " --";
    if (pitch.midiNote >= 0) {
      const int hz = static_cast<int>(std::round(pitch.frequencyHz));
      noteLine = " " + midiNoteName(pitch.midiNote) + "   " +
                 std::to_string(hz) + "Hz";
    }
    screen.writeText(0, row++, fitLine(noteLine, bounds.width), styles.normal);
  }

  if (row < bottom) {
    const int percentage =
        static_cast<int>(std::round(pitch.confidence * 100.0f));
    const int meterWidth = std::max(1, bounds.width - 20);
    const int filled =
        std::clamp(static_cast<int>(std::round(meterWidth * pitch.confidence)),
                   0, meterWidth);
    std::string meter = "[";
    meter.append(static_cast<std::size_t>(filled), '#');
    meter.append(static_cast<std::size_t>(meterWidth - filled), '.');
    meter.push_back(']');
    const std::string confidenceLine =
        " Confidence " + meter + " " + std::to_string(percentage) + "%";
    screen.writeText(0, row++, fitLine(confidenceLine, bounds.width),
                     styles.dim);
  }

  if (row < bottom) {
    std::string statusLine;
    if (!analysis.error.empty()) {
      statusLine = " Analysis: Error - " + analysis.error;
    } else if (analysis.ready) {
      statusLine =
          " Analysis: Ready (" + std::to_string(analysis.frameCount) + " pts)";
    } else if (analysis.running) {
      const int progress =
          static_cast<int>(std::round(analysis.progress * 100.0f));
      statusLine = " Analysis: " + std::to_string(progress) + "%";
    } else {
      statusLine = " Analysis: Idle";
    }
    screen.writeText(0, row++, fitLine(statusLine, bounds.width), styles.dim);
  }

  const int graphTop = row;
  const int graphHeight = bottom - graphTop;
  if (graphHeight < 4) {
    return;
  }

  constexpr int kGraphMinMidi = 36; // C2
  constexpr int kGraphMaxMidi = 96; // C7
  const int labelWidth = bounds.width >= 34 ? 7 : 0;
  const int chartX = labelWidth;
  const int chartWidth = bounds.width - chartX;
  if (chartWidth < 8) {
    return;
  }

  const auto midiToRow = [&](int midiNote) {
    const int clamped = std::clamp(midiNote, kGraphMinMidi, kGraphMaxMidi);
    const float normalized = static_cast<float>(kGraphMaxMidi - clamped) /
                             static_cast<float>(kGraphMaxMidi - kGraphMinMidi);
    const int offset =
        static_cast<int>(std::round(normalized * (graphHeight - 1)));
    return graphTop + std::clamp(offset, 0, graphHeight - 1);
  };

  for (int midiNote = kGraphMinMidi; midiNote <= kGraphMaxMidi;
       midiNote += 12) {
    const int y = midiToRow(midiNote);
    if (labelWidth > 0) {
      std::string label = midiNoteName(midiNote);
      const int labelDisplayWidth = utf8DisplayWidth(label);
      if (labelDisplayWidth < labelWidth) {
        label.insert(label.begin(),
                     static_cast<std::size_t>(labelWidth - labelDisplayWidth),
                     ' ');
      }
      screen.writeText(0, y, utf8TakeDisplayWidth(label, labelWidth),
                       styles.dim);
    }
    screen.writeRun(chartX, y, chartWidth, L'.', styles.dim);
  }

  const std::deque<Sample> &history = model.history();
  const std::size_t start =
      history.size() > static_cast<std::size_t>(chartWidth)
          ? history.size() - static_cast<std::size_t>(chartWidth)
          : 0;
  for (int x = 0; x < chartWidth; ++x) {
    const std::size_t index = start + static_cast<std::size_t>(x);
    if (index >= history.size()) {
      break;
    }
    const Sample &sample = history[index];
    if (sample.midiNote < 0) {
      continue;
    }
    Style pointStyle = styles.dim;
    wchar_t point = L'*';
    if (sample.confidence >= 0.75f) {
      pointStyle = styles.accent;
      point = L'#';
    } else if (sample.confidence >= 0.45f) {
      pointStyle = styles.normal;
    }
    screen.writeChar(chartX + x, midiToRow(sample.midiNote), point, pointStyle);
  }

  if (pitch.midiNote >= 0) {
    screen.writeChar(chartX + chartWidth - 1, midiToRow(pitch.midiNote), L'@',
                     styles.accent);
  }
}

} // namespace tui_melody_visualization
