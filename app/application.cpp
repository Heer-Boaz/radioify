#include "app/application.h"

#include <iostream>
#include <utility>

#include "app/application_runtime.h"
#include "audio/loopsplit/loopsplit_cli.h"
#include "core/runtime_helpers.h"
#include "playback/video/playback.h"
#include "tui/tui.h"
#include "tui/tui_export.h"

int runApplication(Options options) {
  configureFfmpegVideoLog({});

  if (options.extractSheet) {
    return runExtractSheetCli(options, audioPlaybackConfigFor(options));
  }
  if (options.splitLoop) {
    return runSplitLoopCli(options);
  }
  if (options.renderRadio) {
    return runRenderRadioCli(options);
  }

  ApplicationRuntime runtime(options);
  return runTui(std::move(options), runtime);
}
