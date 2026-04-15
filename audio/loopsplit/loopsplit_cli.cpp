#include "loopsplit_cli.h"

#include <algorithm>
#include <filesystem>
#include <string>

#include "loopsplit.h"
#include "media_formats.h"
#include "runtime_helpers.h"

std::pair<std::filesystem::path, std::filesystem::path> resolveSplitOutputPaths(
    const std::filesystem::path& input, const std::string& outArg) {
  const std::string base = toUtf8String(input.stem());
  const std::string stingerSuffix = "_stinger.wav";
  const std::string loopSuffix = "_loop.wav";

  if (outArg.empty()) {
    return {input.parent_path() / (base + stingerSuffix),
            input.parent_path() / (base + loopSuffix)};
  }

  std::filesystem::path outPath(outArg);
  bool directoryHint =
      !outArg.empty() &&
      (outArg.back() == '/' || outArg.back() == '\\');
  if (directoryHint ||
      (std::filesystem::exists(outPath) && std::filesystem::is_directory(outPath))) {
    return {outPath / (base + stingerSuffix), outPath / (base + loopSuffix)};
  }

  std::filesystem::path outDir = outPath.parent_path();
  if (outDir.empty()) {
    outDir = input.parent_path();
  }
  std::string stem = toUtf8String(outPath.stem());
  if (stem.empty()) {
    stem = base;
  }
  return {outDir / (stem + stingerSuffix), outDir / (stem + loopSuffix)};
}

int runSplitLoopCli(const Options& o) {
  if (o.input.empty()) {
    die("split-loop requires an input file path.");
  }

  std::filesystem::path inputPath(o.input);
  requireSupportedAudioInputFile(inputPath);

  auto [stingerOutput, loopOutput] = resolveSplitOutputPaths(inputPath, o.output);
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

  bool ok = splitAudioIntoLoopFiles(inputPath, stingerOutput, loopOutput, config,
                                   &result, &error);
  if (!ok) {
    die(error.empty() ? "Failed to split loop." : error);
  }

  logLine("Loop split complete.");
  logLine("  Input:     " + toUtf8String(inputPath));
  logLine("  Stinger:   " + toUtf8String(stingerOutput));
  logLine("  Main-loop: " + toUtf8String(loopOutput));
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
    logLine("    Stinger: " + toUtf8String(stingerOutput));
  }
  logLine("    Main-loop: " + toUtf8String(loopOutput));
  return 0;
}
