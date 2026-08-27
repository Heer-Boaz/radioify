#include "loopsplit_cli.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include "loopsplit.h"
#include "media_formats.h"
#include "output_paths.h"
#include "runtime_helpers.h"

int runSplitLoopCli(const Options& o) {
  if (o.input.empty()) {
    die("split-loop requires an input file path.");
  }

  std::filesystem::path inputPath = pathFromUtf8String(o.input);
  requireSupportedAudioInputFile(inputPath);

  const LoopSplitOutputPaths outputPaths =
      resolveLoopSplitOutputPaths(inputPath, o.output);
  LoopSplitConfig config;
  config.channels = o.mono ? 1 : 2;
  config.sampleRate = 48000;
  config.trackIndex = std::max(0, o.trackIndex);
  if (o.force50Hz) {
    config.kssOptions.force50Hz = true;
    config.nsfOptions.tempoMode = NsfTempoMode::Pal50;
    config.vgmOptions.playbackHz = VgmPlaybackHz::Hz50;
  }
  LoopSplitResult result;
  std::string error;

  bool ok = splitAudioIntoLoopFiles(inputPath, outputPaths.stinger,
                                    outputPaths.loop, config, &result, {}, {},
                                    &error);
  if (!ok) {
    die(error.empty() ? "Failed to split loop." : error);
  }

  logLine("Loop split complete.");
  logLine("  Input:     " + toUtf8String(inputPath));
  logLine("  Stinger:   " + toUtf8String(outputPaths.stinger));
  logLine("  Main-loop: " + toUtf8String(outputPaths.loop));
  if (result.hasLoop) {
    logLine("  Loop start: " + std::to_string(result.loopStartFrame) +
            " frames");
    logLine("  Loop len:   " + std::to_string(result.loopFrameCount) +
            " frames");
    logLine("  Loop conf:  " + std::to_string(result.confidence));
    if (!result.hasStinger) {
      logLine("  Note:      No reliable stinger detected; "
              "exported full audio as loop.");
    }
  } else {
    logLine("  Note:      No reliable loop detected; exported full audio as loop.");
  }
  logLine("  Output:");
  if (result.hasStinger) {
    logLine("    Stinger: " + toUtf8String(outputPaths.stinger));
  }
  logLine("    Main-loop: " + toUtf8String(outputPaths.loop));
  return 0;
}
