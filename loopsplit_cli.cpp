#include "loopsplit_cli.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include "loopsplit.h"

namespace {

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4" || ext == ".mov" || ext == ".kss" ||
         ext == ".nsf" ||
#if !RADIOIFY_DISABLE_GSF_GPL
         ext == ".gsf" || ext == ".minigsf" ||
#endif
         ext == ".mid" || ext == ".midi" || ext == ".vgm" || ext == ".vgz" ||
         ext == ".psf" || ext == ".minipsf" || ext == ".psf2" ||
         ext == ".minipsf2";
}

void validateInputFile(const std::filesystem::path& p) {
  if (p.empty()) die("Missing input file path.");
  if (!std::filesystem::exists(p)) die("Input file not found: " + p.string());
  if (std::filesystem::is_directory(p)) die("Input path must be a file: " + p.string());
  if (!isSupportedAudioExt(p)) {
    die("Unsupported input format '" + p.extension().string() +
        "'. Supported: .wav, .mp3, .flac, .m4a, .webm, .mp4, .mov, .kss, .nsf, "
#if !RADIOIFY_DISABLE_GSF_GPL
        ".gsf, .minigsf, "
#endif
        ".mid, .midi, .vgm, .vgz, .psf, .minipsf, .psf2, .minipsf2.");
  }
}

}  // namespace

std::pair<std::filesystem::path, std::filesystem::path> resolveSplitOutputPaths(
    const std::filesystem::path& input, const std::string& outArg) {
  const std::string base = input.stem().string();
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
  std::string stem = outPath.stem().string();
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
  validateInputFile(inputPath);

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
  logLine("  Input:     " + inputPath.string());
  logLine("  Stinger:   " + stingerOutput.string());
  logLine("  Main-loop: " + loopOutput.string());
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
    logLine("    Stinger: " + stingerOutput.string());
  }
  logLine("    Main-loop: " + loopOutput.string());
  return 0;
}
