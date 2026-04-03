#ifndef RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_PASS_BUILDER_H
#define RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_PASS_BUILDER_H

#include "audio_graph.h"

#include <initializer_list>
#include <stdexcept>
#include <string_view>

template <typename Engine,
          typename BlockControl,
          typename SampleContext,
          typename Id,
          size_t MaxDependencies = 4>
class AudioPassBuilder {
 public:
  using Graph = AudioGraphRuntime<Engine,
                                  BlockControl,
                                  SampleContext,
                                  Id,
                                  MaxDependencies>;
  using Pass = typename Graph::Pass;

  static AudioPassBuilder block(Id id, std::string_view name) {
    return AudioPassBuilder(AudioPassDomain::Block, id, name);
  }

  static AudioPassBuilder control(Id id, std::string_view name) {
    return AudioPassBuilder(AudioPassDomain::SampleControl, id, name);
  }

  static AudioPassBuilder program(Id id, std::string_view name) {
    return AudioPassBuilder(AudioPassDomain::Program, id, name);
  }

  AudioPassBuilder& enabled(bool value) {
    m_pass.enabled = value;
    return *this;
  }

  AudioPassBuilder& stateOnly(bool value = true) {
    m_pass.stateOnly = value;
    return *this;
  }

  AudioPassBuilder& dependsOn(Id id) {
    m_pass.dependencies.add(id);
    return *this;
  }

  AudioPassBuilder& dependsOn(std::initializer_list<Id> ids) {
    for (Id id : ids) {
      m_pass.dependencies.add(id);
    }
    return *this;
  }

  AudioPassBuilder& prepare(typename Pass::BlockPrepareFn fn) {
    m_pass.prepare = fn;
    return *this;
  }

  AudioPassBuilder& run(typename Pass::ControlRunFn fn) {
    m_pass.runControl = fn;
    return *this;
  }

  AudioPassBuilder& run(typename Pass::SampleRunFn fn) {
    m_pass.runSample = fn;
    return *this;
  }

  Pass build() const {
    validate();
    return m_pass;
  }

  void addTo(Graph& graph) const { graph.addPass(build()); }

 private:
  AudioPassBuilder(AudioPassDomain domain, Id id, std::string_view name) {
    m_pass.domain = domain;
    m_pass.id = id;
    m_pass.name = name;
  }

  void validate() const {
    switch (m_pass.domain) {
      case AudioPassDomain::Block:
        if (!m_pass.prepare) {
          throw std::runtime_error("Audio block pass requires prepare callback");
        }
        return;
      case AudioPassDomain::SampleControl:
        if (!m_pass.runControl) {
          throw std::runtime_error(
              "Audio control pass requires run callback");
        }
        return;
      case AudioPassDomain::Program:
        if (!m_pass.runSample) {
          throw std::runtime_error(
              "Audio program pass requires run callback");
        }
        return;
    }
  }

  Pass m_pass{};
};

#endif  // RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_PASS_BUILDER_H
