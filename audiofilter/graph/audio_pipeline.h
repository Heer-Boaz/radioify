#ifndef RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_PIPELINE_H
#define RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_PIPELINE_H

#include "audio_graph.h"
#include "audio_lifecycle.h"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

template <typename Engine,
          typename BlockControl,
          typename SampleContext,
          typename InitContext,
          typename Id,
          size_t MaxDependencies = 4>
struct AudioPipelinePass {
  using Graph = AudioGraphRuntime<Engine,
                                  BlockControl,
                                  SampleContext,
                                  Id,
                                  MaxDependencies>;
  using GraphPass = typename Graph::Pass;
  using Lifecycle = AudioLifecycleRuntime<Engine, InitContext, Id>;

  Id id{};
  std::string_view name{};
  AudioPassDomain domain = AudioPassDomain::Program;
  bool stateOnly = false;
  AudioPassDependencies<Id, MaxDependencies> dependencies{};
  typename Lifecycle::InitFn init = nullptr;
  typename Lifecycle::ResetFn reset = nullptr;
  typename GraphPass::BlockPrepareFn prepare = nullptr;
  typename GraphPass::ControlRunFn runControl = nullptr;
  typename GraphPass::SampleRunFn runSample = nullptr;
};

template <typename Engine,
          typename BlockControl,
          typename SampleContext,
          typename InitContext,
          typename Id,
          size_t MaxDependencies = 4>
class AudioPipelinePassBuilder {
 public:
  using Pass = AudioPipelinePass<Engine,
                                 BlockControl,
                                 SampleContext,
                                 InitContext,
                                 Id,
                                 MaxDependencies>;

  AudioPipelinePassBuilder(std::vector<Pass>* passes, size_t index)
      : m_passes(passes), m_index(index) {}

  AudioPipelinePassBuilder& init(typename Pass::Lifecycle::InitFn fn) {
    pass().init = fn;
    return *this;
  }

  AudioPipelinePassBuilder& reset(typename Pass::Lifecycle::ResetFn fn) {
    pass().reset = fn;
    return *this;
  }

  AudioPipelinePassBuilder& stateOnly(bool value = true) {
    pass().stateOnly = value;
    return *this;
  }

  AudioPipelinePassBuilder& dependsOn(Id id) {
    pass().dependencies.add(id);
    return *this;
  }

  AudioPipelinePassBuilder& prepare(typename Pass::GraphPass::BlockPrepareFn fn) {
    pass().prepare = fn;
    return *this;
  }

  AudioPipelinePassBuilder& run(typename Pass::GraphPass::ControlRunFn fn) {
    pass().runControl = fn;
    return *this;
  }

  AudioPipelinePassBuilder& run(typename Pass::GraphPass::SampleRunFn fn) {
    pass().runSample = fn;
    return *this;
  }

 private:
  Pass& pass() { return (*m_passes)[m_index]; }

  std::vector<Pass>* m_passes = nullptr;
  size_t m_index = 0;
};

template <typename Engine,
          typename BlockControl,
          typename SampleContext,
          typename InitContext,
          typename Id,
          size_t MaxDependencies = 4>
class AudioPipeline {
 public:
  using Pass = AudioPipelinePass<Engine,
                                 BlockControl,
                                 SampleContext,
                                 InitContext,
                                 Id,
                                 MaxDependencies>;
  using Graph = typename Pass::Graph;
  using Lifecycle = typename Pass::Lifecycle;

  explicit AudioPipeline(std::vector<Pass> passes)
      : m_passes(std::move(passes)) {}

  const Pass* findPass(Id id) const {
    for (const auto& pass : m_passes) {
      if (pass.id == id) return &pass;
    }
    return nullptr;
  }

  std::string_view passName(Id id) const {
    if (const auto* pass = findPass(id)) return pass->name;
    return {};
  }

  Graph buildGraph() const {
    Graph graph;
    for (const auto& pass : m_passes) {
      typename Graph::Pass graphPass;
      graphPass.domain = pass.domain;
      graphPass.id = pass.id;
      graphPass.name = pass.name;
      graphPass.enabled = true;
      graphPass.stateOnly = pass.stateOnly;
      graphPass.dependencies = pass.dependencies;
      graphPass.prepare = pass.prepare;
      graphPass.runControl = pass.runControl;
      graphPass.runSample = pass.runSample;
      graph.addPass(graphPass);
    }
    return graph;
  }

  Lifecycle buildLifecycle() const {
    Lifecycle lifecycle;
    for (const auto& pass : m_passes) {
      lifecycle.addInit(pass.id, pass.name, pass.init);
    }
    for (size_t i = m_passes.size(); i > 0; --i) {
      const auto& pass = m_passes[i - 1];
      lifecycle.addReset(pass.id, pass.name, pass.reset);
    }
    return lifecycle;
  }

 private:
  std::vector<Pass> m_passes{};
};

template <typename Engine,
          typename BlockControl,
          typename SampleContext,
          typename InitContext,
          typename Id,
          size_t MaxDependencies = 4>
class AudioPipelineBuilder {
 public:
  using Pass = AudioPipelinePass<Engine,
                                 BlockControl,
                                 SampleContext,
                                 InitContext,
                                 Id,
                                 MaxDependencies>;
  using PassBuilder = AudioPipelinePassBuilder<Engine,
                                               BlockControl,
                                               SampleContext,
                                               InitContext,
                                               Id,
                                               MaxDependencies>;
  using Pipeline = AudioPipeline<Engine,
                                 BlockControl,
                                 SampleContext,
                                 InitContext,
                                 Id,
                                 MaxDependencies>;

  PassBuilder block(Id id, std::string_view name) {
    return addPass(AudioPassDomain::Block, id, name);
  }

  PassBuilder control(Id id, std::string_view name) {
    return addPass(AudioPassDomain::SampleControl, id, name);
  }

  PassBuilder program(Id id, std::string_view name) {
    return addPass(AudioPassDomain::Program, id, name);
  }

  Pipeline build(size_t expectedPassCount) const {
    validate(expectedPassCount);
    return Pipeline(m_passes);
  }

 private:
  PassBuilder addPass(AudioPassDomain domain, Id id, std::string_view name) {
    Pass pass;
    pass.id = id;
    pass.name = name;
    pass.domain = domain;
    m_passes.push_back(pass);
    return PassBuilder(&m_passes, m_passes.size() - 1);
  }

  void validate(size_t expectedPassCount) const {
    std::vector<bool> seen(expectedPassCount, false);
    for (const auto& pass : m_passes) {
      const size_t index = static_cast<size_t>(pass.id);
      if (index >= expectedPassCount) {
        throw std::runtime_error("Audio pipeline pass uses invalid pass id");
      }
      if (seen[index]) {
        throw std::runtime_error("Duplicate audio pipeline pass id");
      }
      seen[index] = true;
      if (!pass.init) {
        throw std::runtime_error("Audio pipeline pass missing init callback");
      }
      if (!pass.reset) {
        throw std::runtime_error("Audio pipeline pass missing reset callback");
      }
      switch (pass.domain) {
        case AudioPassDomain::Block:
          if (!pass.prepare) {
            throw std::runtime_error(
                "Audio block pipeline pass missing prepare callback");
          }
          break;
        case AudioPassDomain::SampleControl:
          if (!pass.runControl) {
            throw std::runtime_error(
                "Audio control pipeline pass missing run callback");
          }
          break;
        case AudioPassDomain::Program:
          if (!pass.runSample) {
            throw std::runtime_error(
                "Audio program pipeline pass missing run callback");
          }
          break;
      }
    }

    for (bool present : seen) {
      if (!present) {
        throw std::runtime_error(
            "Audio pipeline definition does not cover all passes");
      }
    }
  }

  std::vector<Pass> m_passes{};
};

#endif  // RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_PIPELINE_H
