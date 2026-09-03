#include "app/application.h"

#include <iostream>
#include <utility>

#include "app/chapter_analysis_cli.h"
#include "app/application_runtime.h"
#include "audio/loopsplit/loopsplit_cli.h"
#include "core/runtime_helpers.h"
#include "playback/video/chapter/model.h"
#include "playback/video/playback.h"
#include "tui/tui.h"
#include "tui/tui_export.h"

namespace {

const char *capabilityName(
    playback_video_chapters::CapabilityState state) {
  using playback_video_chapters::CapabilityState;
  switch (state) {
  case CapabilityState::Ready:
    return "ready";
  case CapabilityState::SetupRequired:
    return "setup-required";
  case CapabilityState::Unsupported:
    return "unsupported";
  case CapabilityState::Cancelled:
    return "cancelled";
  case CapabilityState::Yielded:
    return "yielded";
  }
  return "unknown";
}

int verifyChapterModels() {
  using namespace playback_video_chapters;
  const ModelPaths paths = resolveModelPaths();
  const CapabilityResult result = inspectModelArtifacts(paths, {});
  std::cout << "state=" << capabilityName(result.state) << '\n'
            << "package_root=" << toUtf8String(radioifyInstalledPackageDir())
            << '\n'
            << "executable_root=" << toUtf8String(radioifyExecutableDir())
            << '\n'
            << "planner_adapter=" << toUtf8String(paths.plannerAdapter)
            << '\n';
  if (!result.detail.empty())
    std::cout << "detail=" << result.detail << '\n';
  return result.state == CapabilityState::Ready ? 0 : 1;
}

int verifyChapterRuntime() {
  using namespace playback_video_chapters;
  const CapabilityResult result = inspectPackagedChapterRuntime();
  const std::filesystem::path adapter =
      radioifyExecutableDir() / "models" / "chapter_analysis" /
      "chapter-llama-captions-asr-10k-f16.gguf";
  std::cout << "state=" << capabilityName(result.state) << '\n'
            << "executable_root=" << toUtf8String(radioifyExecutableDir())
            << '\n'
            << "planner_adapter=" << toUtf8String(adapter) << '\n';
  if (!result.detail.empty())
    std::cout << "detail=" << result.detail << '\n';
  return result.state == CapabilityState::Ready ? 0 : 1;
}

} // namespace

int runApplication(Options options) {
  configureFfmpegVideoLog({});

  if (options.verifyChapterModels) {
    return verifyChapterModels();
  }
  if (options.verifyChapterRuntime) {
    return verifyChapterRuntime();
  }
  if (options.analyzeChapters) {
    return runChapterAnalysisCli(pathFromUtf8String(options.input));
  }

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
