#ifndef RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_GRAPH_H
#define RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_GRAPH_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

enum class AudioPassDomain : uint8_t {
  Block,
  SampleControl,
  Program,
};

template <typename Id, size_t MaxDependencies = 4>
struct AudioPassDependencies {
  std::array<Id, MaxDependencies> ids{};
  size_t count = 0;

  void add(Id id) {
    if (count >= MaxDependencies) {
      throw std::runtime_error("Audio pass dependency limit exceeded");
    }
    ids[count++] = id;
  }
};

template <typename Engine,
          typename BlockControl,
          typename SampleContext,
          typename Id,
          size_t MaxDependencies = 4>
struct AudioGraphPass {
  using BlockPrepareFn = void (*)(Engine&, BlockControl&, uint32_t);
  using ControlRunFn = void (*)(Engine&, SampleContext&);
  using SampleRunFn = float (*)(Engine&, float, SampleContext&);

  AudioPassDomain domain = AudioPassDomain::Program;
  Id id{};
  std::string_view name{};
  bool enabled = true;
  bool stateOnly = false;
  AudioPassDependencies<Id, MaxDependencies> dependencies{};
  BlockPrepareFn prepare = nullptr;
  ControlRunFn runControl = nullptr;
  SampleRunFn runSample = nullptr;
};

template <typename Engine,
          typename BlockControl,
          typename SampleContext,
          typename Id,
          size_t MaxDependencies = 4>
class AudioGraphRuntime {
 public:
  using Pass = AudioGraphPass<Engine,
                              BlockControl,
                              SampleContext,
                              Id,
                              MaxDependencies>;

  bool bypass = false;

  void addPass(const Pass& pass) {
    if (m_compiled) {
      throw std::runtime_error("Cannot add audio passes after compilation");
    }
    m_passes.push_back(pass);
  }

  Pass* findPass(Id id) {
    for (auto& pass : m_passes) {
      if (pass.id == id) return &pass;
    }
    return nullptr;
  }

  const Pass* findPass(Id id) const {
    for (const auto& pass : m_passes) {
      if (pass.id == id) return &pass;
    }
    return nullptr;
  }

  bool isEnabled(Id id) const {
    if (const auto* pass = findPass(id)) return pass->enabled;
    return false;
  }

  void setEnabled(Id id, bool value) {
    if (auto* pass = findPass(id)) {
      if (pass->enabled != value) {
        pass->enabled = value;
        invalidate();
      }
    }
  }

  void invalidate() {
    m_compiled = false;
    m_blockOrder.clear();
    m_sampleControlOrder.clear();
    m_programOrder.clear();
    m_blockOrderIndex.clear();
    m_sampleControlOrderIndex.clear();
    m_programOrderIndex.clear();
  }

  void compile() {
    if (m_compiled) return;
    m_blockOrder = compileDomainOrder(AudioPassDomain::Block);
    m_sampleControlOrder = compileDomainOrder(AudioPassDomain::SampleControl);
    m_programOrder = compileDomainOrder(AudioPassDomain::Program);
    m_blockOrderIndex = buildOrderIndexLookup(m_blockOrder);
    m_sampleControlOrderIndex = buildOrderIndexLookup(m_sampleControlOrder);
    m_programOrderIndex = buildOrderIndexLookup(m_programOrder);
    m_compiled = true;
  }

  const std::vector<size_t>& order(AudioPassDomain domain) const {
    switch (domain) {
      case AudioPassDomain::Block:
        return m_blockOrder;
      case AudioPassDomain::SampleControl:
        return m_sampleControlOrder;
      case AudioPassDomain::Program:
        return m_programOrder;
    }
    return m_programOrder;
  }

  const Pass& pass(size_t index) const { return m_passes[index]; }

  size_t findOrderIndex(AudioPassDomain domain, Id id) const {
    const auto* passPtr = findPass(id);
    if (!passPtr) return order(domain).size();
    const size_t passIndex =
        static_cast<size_t>(passPtr - m_passes.data());
    const auto& lookup = orderIndexLookup(domain);
    if (passIndex < lookup.size()) return lookup[passIndex];
    return order(domain).size();
  }

  template <typename Fn>
  void forEachOrderedPass(AudioPassDomain domain, Fn&& fn) const {
    const auto& compiledOrder = order(domain);
    for (size_t passIndex : compiledOrder) {
      fn(m_passes[passIndex]);
    }
  }

 private:
  static constexpr size_t kInvalidOrderIndex = static_cast<size_t>(-1);

  std::vector<size_t> buildOrderIndexLookup(
      const std::vector<size_t>& compiledOrder) const {
    std::vector<size_t> lookup(m_passes.size(), kInvalidOrderIndex);
    for (size_t orderIndex = 0; orderIndex < compiledOrder.size(); ++orderIndex) {
      lookup[compiledOrder[orderIndex]] = orderIndex;
    }
    return lookup;
  }

  const std::vector<size_t>& orderIndexLookup(AudioPassDomain domain) const {
    switch (domain) {
      case AudioPassDomain::Block:
        return m_blockOrderIndex;
      case AudioPassDomain::SampleControl:
        return m_sampleControlOrderIndex;
      case AudioPassDomain::Program:
        return m_programOrderIndex;
    }
    return m_programOrderIndex;
  }

  std::vector<size_t> compileDomainOrder(AudioPassDomain domain) const {
    std::vector<size_t> enabledIndices;
    enabledIndices.reserve(m_passes.size());
    for (size_t i = 0; i < m_passes.size(); ++i) {
      if (m_passes[i].domain == domain && m_passes[i].enabled) {
        enabledIndices.push_back(i);
      }
    }

    std::vector<int> indegree(m_passes.size(), 0);
    std::vector<std::vector<size_t>> adjacency(m_passes.size());
    for (size_t passIndex : enabledIndices) {
      const auto& pass = m_passes[passIndex];
      for (size_t dependencyIndex = 0;
           dependencyIndex < pass.dependencies.count;
           ++dependencyIndex) {
        const Id dependencyId = pass.dependencies.ids[dependencyIndex];
        for (size_t candidateIndex : enabledIndices) {
          if (m_passes[candidateIndex].id == dependencyId) {
            adjacency[candidateIndex].push_back(passIndex);
            indegree[passIndex]++;
            break;
          }
        }
      }
    }

    std::vector<size_t> queue;
    queue.reserve(enabledIndices.size());
    for (size_t passIndex : enabledIndices) {
      if (indegree[passIndex] == 0) {
        queue.push_back(passIndex);
      }
    }

    std::vector<size_t> compiledOrder;
    compiledOrder.reserve(enabledIndices.size());
    for (size_t head = 0; head < queue.size(); ++head) {
      const size_t current = queue[head];
      compiledOrder.push_back(current);
      for (size_t dependent : adjacency[current]) {
        if (--indegree[dependent] == 0) {
          queue.push_back(dependent);
        }
      }
    }

    if (compiledOrder.size() != enabledIndices.size()) {
      throw std::runtime_error("Audio pipeline cycle detected");
    }

    return compiledOrder;
  }

  std::vector<Pass> m_passes{};
  bool m_compiled = false;
  std::vector<size_t> m_blockOrder{};
  std::vector<size_t> m_sampleControlOrder{};
  std::vector<size_t> m_programOrder{};
  std::vector<size_t> m_blockOrderIndex{};
  std::vector<size_t> m_sampleControlOrderIndex{};
  std::vector<size_t> m_programOrderIndex{};
};

#endif  // RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_GRAPH_H
