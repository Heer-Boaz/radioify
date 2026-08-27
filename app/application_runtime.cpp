#include "app/application_runtime.h"

#include "app/playback_route.h"
#include "playback/target_resolver.h"

AudioPlaybackConfig audioPlaybackConfigFor(const Options& options) {
  AudioPlaybackConfig config;
  config.enableAudio = options.enableAudio;
  config.enableRadio = options.enableRadio;
  config.mono = options.mono;
  config.dry = options.dry;
  config.radioSettingsPath = options.radioSettingsPath;
  config.radioPresetName = options.radioPresetName;
  config.radioReceiverProfile = options.radioReceiverProfile;
  config.radioReceptionProfile = options.radioReceptionProfile;
  config.bwHz = options.bwHz;
  config.noise = options.noise;
  return config;
}

ApplicationRuntime::ApplicationRuntime(const Options& options)
    : gpu_(),
      audioPlayback_(audioPlaybackConfigFor(options)),
      playbackQueue_(
          {[](const std::filesystem::path& file) {
             return playback_target_resolver::resolvePlaybackTarget(file);
           },
           [](const PlaybackTarget& target) {
             return playback_route::resolveTarget(target);
           }}),
      mediaProcessing_(audioPlayback_),
      mediaActions_(mediaProcessing_) {}

ApplicationRuntime::~ApplicationRuntime() = default;
