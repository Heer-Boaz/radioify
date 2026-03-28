#ifndef RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_LIFECYCLE_H
#define RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_LIFECYCLE_H

#include <string_view>
#include <vector>

template <typename Engine, typename InitContext, typename Id>
class AudioLifecycleRuntime {
 public:
  using InitFn = void (*)(Engine&, InitContext&);
  using ResetFn = void (*)(Engine&);

  struct InitStep {
    Id id{};
    std::string_view name{};
    InitFn init = nullptr;
  };

  struct ResetStep {
    Id id{};
    std::string_view name{};
    ResetFn reset = nullptr;
  };

  void addInit(Id id, std::string_view name, InitFn init) {
    initSteps.push_back({id, name, init});
  }

  void addReset(Id id, std::string_view name, ResetFn reset) {
    resetSteps.push_back({id, name, reset});
  }

  void initialize(Engine& engine, InitContext& initCtx) const {
    for (const auto& step : initSteps) {
      if (step.init) step.init(engine, initCtx);
    }
  }

  void reset(Engine& engine) const {
    for (const auto& step : resetSteps) {
      if (step.reset) step.reset(engine);
    }
  }

 private:
  std::vector<InitStep> initSteps{};
  std::vector<ResetStep> resetSteps{};
};

#endif  // RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_LIFECYCLE_H
