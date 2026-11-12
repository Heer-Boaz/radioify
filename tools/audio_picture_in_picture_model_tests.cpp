#include "playback/overlay/overlay.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <variant>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "audio_picture_in_picture_model_tests: " << message << '\n';
  return condition;
}

}  // namespace

int main() {
  namespace overlay = playback_overlay;
  bool ok = true;

  overlay::PlaybackOverlayInputs inputs;
  inputs.playPauseAvailable = true;
  inputs.radioVisualizationAvailable = true;
  inputs.radioVisualizationActive = true;
  inputs.pictureInPictureAvailable = true;
  inputs.pictureInPictureActive = true;
  auto state = overlay::buildPlaybackOverlayState(inputs);
  overlay::OverlayControlSpecOptions options;
  options.includeAudioTrack = false;
  options.includeSubtitles = false;
  const auto visualId = overlay::OverlayControlId::RadioVisualization;
  const int visualToken = overlay::overlayControlToken(visualId);
  const auto findControl = [](const auto& specs, overlay::OverlayControlId id) {
    return std::find_if(specs.begin(), specs.end(),
                        [&](const auto& spec) { return spec.id == id; });
  };
  const auto intent = overlay::intentForOverlayControl(visualId);
  const auto* action = std::get_if<overlay::OverlayAction>(&intent);
  ok &= expect(action && *action == overlay::OverlayAction::ToggleRadioVisualization,
               "the visualization control must have a typed toggle intent");

  // The layout and hit map consume the same projected controls. Vary the
  // transport capabilities so no fixed list index can determine placement.
  for (const bool fullTransport : {false, true}) {
    state.canPlayPrevious = fullTransport;
    state.canPlayNext = fullTransport;
    const auto specs = overlay::buildOverlayControlSpecs(state, visualToken, options);
    const auto visual = findControl(specs, visualId);
    const auto radio = findControl(specs, overlay::OverlayControlId::Radio);
    ok &= expect(visual != specs.end() && radio != specs.end() &&
                     visual < radio && visual->active && visual->enabled &&
                     visual->renderText == visual->hoverText,
                 "the active visualization control must precede the radio "
                 "regardless of transport availability");

    for (const int width : {28, 80}) {
      overlay::OverlayCellLayoutInput layoutInput;
      layoutInput.width = width;
      layoutInput.height = 8;
      layoutInput.title = "A very long track title that occupies several rows "
                          "unless the layout reserves room for the visualization";
      layoutInput.maximumTitleLines = 1;
      layoutInput.reservedRowsAboveProgress = 1;
      layoutInput.controls = overlay::buildOverlayCellControlInputs(specs, visualToken);
      const auto layout = overlay::layoutOverlayCells(layoutInput);
      ok &= expect(layout.titleLines.size() == 1 && layout.topY > 0 &&
                       layout.titleLines.front().text.back() == '~',
                   "a compact PiP must reserve room for the image and mark "
                   "a truncated title");
      const auto placed = std::find_if(
          layout.controls.begin(), layout.controls.end(),
          [&](const auto& control) { return control.id == visualId; });
      if (placed == layout.controls.end()) {
        ok &= expect(false, "the visualization button disappeared from PiP");
        continue;
      }
      const auto hit = overlay::interactionHitAt(
          overlay::buildOverlayInteractionMap(layout),
          placed->x + 0.5, placed->y + 0.5);
      ok &= expect(hit.control == visualId,
                   "the visible visualization button must be clickable at "
                   "both narrow and wide PiP sizes");
    }
  }

  inputs.radioVisualizationAvailable = false;
  state = overlay::buildPlaybackOverlayState(inputs);
  const auto withoutVisualization = overlay::buildOverlayControlSpecs(state, -1, options);
  ok &= expect(findControl(withoutVisualization, visualId) == withoutVisualization.end(),
               "surfaces without a receiver view must not show its control");
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
