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
    for (auto& schedule : m_schedules) schedule = {};
  }

  void compile() {
    if (m_compiled) return;
    for (auto domain : {AudioPassDomain::Block, AudioPassDomain::SampleControl,
                        AudioPassDomain::Program}) {
      schedule(domain) = compileDomainSchedule(domain);
    }
    m_compiled = true;
  }

  // Enabled passes, in the order they run.
  const std::vector<size_t>& order(AudioPassDomain domain) const {
    return schedule(domain).active;
  }

  const Pass& pass(size_t index) const { return m_passes[index]; }

  size_t findOrderIndex(AudioPassDomain domain, Id id) const {
    const auto* passPtr = findPass(id);
    const auto& compiled = schedule(domain);
    if (!passPtr) return compiled.active.size();
    const size_t passIndex = static_cast<size_t>(passPtr - m_passes.data());
    if (passIndex < compiled.activeIndex.size() &&
        compiled.activeIndex[passIndex] != kInvalidOrderIndex)
      return compiled.activeIndex[passIndex];
    return compiled.active.size();
  }

  // Entry point for a signal that enters at `id`. An enabled pass starts at
  // its own position; a disabled one at the next enabled pass in the full
  // dependency order. Returns order(domain).size() when none remains.
  size_t findEntryOrderIndex(AudioPassDomain domain, Id id) const {
    const auto* passPtr = findPass(id);
    const auto& compiled = schedule(domain);
    if (!passPtr) return compiled.active.size();
    const size_t passIndex = static_cast<size_t>(passPtr - m_passes.data());
    if (passIndex < compiled.entryIndex.size()) return compiled.entryIndex[passIndex];
    return compiled.active.size();
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

  // The dependency order is solved over every pass of the domain, enabled or
  // not. Disabling a pass removes it from `active` without dropping the
  // ordering its dependency edges imposed on the passes that remain.
  struct DomainSchedule {
    std::vector<size_t> active{};
    // Per registered pass: position in `active`, or kInvalidOrderIndex.
    std::vector<size_t> activeIndex{};
    // Per registered pass: first `active` position at or after it in the
    // full order, or active.size().
    std::vector<size_t> entryIndex{};
  };

  DomainSchedule& schedule(AudioPassDomain domain) {
    return m_schedules[static_cast<size_t>(domain)];
  }
  const DomainSchedule& schedule(AudioPassDomain domain) const {
    return m_schedules[static_cast<size_t>(domain)];
  }

  DomainSchedule compileDomainSchedule(AudioPassDomain domain) const {
    const std::vector<size_t> full = compileFullDomainOrder(domain);
    DomainSchedule result;
    result.activeIndex.assign(m_passes.size(), kInvalidOrderIndex);
    result.entryIndex.assign(m_passes.size(), kInvalidOrderIndex);
    for (size_t passIndex : full) {
      if (!m_passes[passIndex].enabled) continue;
      result.activeIndex[passIndex] = result.active.size();
      result.active.push_back(passIndex);
    }
    size_t next = result.active.size();
    for (size_t position = full.size(); position-- > 0;) {
      const size_t passIndex = full[position];
      if (result.activeIndex[passIndex] != kInvalidOrderIndex)
        next = result.activeIndex[passIndex];
      result.entryIndex[passIndex] = next;
    }
    return result;
  }

  std::vector<size_t> compileFullDomainOrder(AudioPassDomain domain) const {
    std::vector<size_t> domainIndices;
    domainIndices.reserve(m_passes.size());
    for (size_t i = 0; i < m_passes.size(); ++i) {
      if (m_passes[i].domain == domain) {
        domainIndices.push_back(i);
      }
    }

    std::vector<int> indegree(m_passes.size(), 0);
    std::vector<std::vector<size_t>> adjacency(m_passes.size());
    for (size_t passIndex : domainIndices) {
      const auto& pass = m_passes[passIndex];
      for (size_t dependencyIndex = 0;
           dependencyIndex < pass.dependencies.count;
           ++dependencyIndex) {
        const Id dependencyId = pass.dependencies.ids[dependencyIndex];
        for (size_t candidateIndex : domainIndices) {
          if (m_passes[candidateIndex].id == dependencyId) {
            adjacency[candidateIndex].push_back(passIndex);
            indegree[passIndex]++;
            break;
          }
        }
      }
    }

    std::vector<size_t> queue;
    queue.reserve(domainIndices.size());
    for (size_t passIndex : domainIndices) {
      if (indegree[passIndex] == 0) {
        queue.push_back(passIndex);
      }
    }

    std::vector<size_t> compiledOrder;
    compiledOrder.reserve(domainIndices.size());
    for (size_t head = 0; head < queue.size(); ++head) {
      const size_t current = queue[head];
      compiledOrder.push_back(current);
      for (size_t dependent : adjacency[current]) {
        if (--indegree[dependent] == 0) {
          queue.push_back(dependent);
        }
      }
    }

    if (compiledOrder.size() != domainIndices.size()) {
      throw std::runtime_error("Audio pipeline cycle detected");
    }

    return compiledOrder;
  }

  std::vector<Pass> m_passes{};
  bool m_compiled = false;
  std::array<DomainSchedule, 3> m_schedules{};
};

#endif  // RADIOIFY_AUDIOFILTER_GRAPH_AUDIO_GRAPH_H
