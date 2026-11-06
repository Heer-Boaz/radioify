#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <vector>

#include "audiofilter/graph/audio_graph.h"

namespace {
struct Engine {};
struct BlockControl {};
struct SampleContext {};
enum class Id { A, B, C, D, Input, FrontEnd, Mixer, Missing };
using Graph = AudioGraphRuntime<Engine, BlockControl, SampleContext, Id>;

void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "audio_graph_tests: " << message << '\n';
    std::exit(1);
  }
}

void add(Graph& graph, Id id, std::initializer_list<Id> dependencies = {}) {
  Graph::Pass pass;
  pass.id = id;
  for (Id dependency : dependencies) pass.dependencies.add(dependency);
  graph.addPass(pass);
}

std::vector<Id> activeIds(Graph& graph) {
  graph.compile();
  std::vector<Id> ids;
  graph.forEachOrderedPass(AudioPassDomain::Program,
                           [&](const auto& pass) { ids.push_back(pass.id); });
  return ids;
}

size_t entry(Graph& graph, Id id) {
  graph.compile();
  return graph.findEntryOrderIndex(AudioPassDomain::Program, id);
}

// Registration order differs from execution order: Input -> FrontEnd -> Mixer.
Graph receiverGraph() {
  Graph graph;
  add(graph, Id::FrontEnd, {Id::Input});
  add(graph, Id::Input);
  add(graph, Id::Mixer, {Id::FrontEnd});
  return graph;
}

void testRegistrationOrderIsNotExecutionOrder() {
  auto graph = receiverGraph();
  check(activeIds(graph) == std::vector<Id>{Id::Input, Id::FrontEnd, Id::Mixer},
        "dependency order ignored");
  check(entry(graph, Id::FrontEnd) == 1,
        "enabled entry resolved to a pass registered after it");
}

void testEnabledEntryKeepsItsPosition() {
  auto graph = receiverGraph();
  check(entry(graph, Id::Input) == 0 && entry(graph, Id::Mixer) == 2,
        "enabled entry did not keep its own position");
  check(entry(graph, Id::Missing) == activeIds(graph).size(),
        "unknown entry must run nothing");
}

void testDisabledEntryStartsAtNextEnabledPass() {
  auto graph = receiverGraph();
  graph.setEnabled(Id::FrontEnd, false);
  check(activeIds(graph) == std::vector<Id>{Id::Input, Id::Mixer},
        "disabling the front end changed the remaining order");
  check(entry(graph, Id::FrontEnd) == 1, "disabled entry did not continue at the mixer");
  check(entry(graph, Id::Input) == 0, "enabled predecessor moved");
}

void testConsecutiveDisabledEntries() {
  Graph graph;
  add(graph, Id::D, {Id::C});
  add(graph, Id::B, {Id::A});
  add(graph, Id::C, {Id::B});
  add(graph, Id::A);
  graph.setEnabled(Id::B, false);
  graph.setEnabled(Id::C, false);
  check(activeIds(graph) == std::vector<Id>{Id::A, Id::D}, "remaining order changed");
  check(entry(graph, Id::B) == 1 && entry(graph, Id::C) == 1,
        "consecutive disabled entries did not reach the next enabled pass");
}

void testNoRemainingEnabledPass() {
  auto graph = receiverGraph();
  graph.setEnabled(Id::FrontEnd, false);
  graph.setEnabled(Id::Mixer, false);
  check(entry(graph, Id::FrontEnd) == activeIds(graph).size() &&
            entry(graph, Id::Mixer) == activeIds(graph).size(),
        "entry without a remaining enabled pass must run nothing");
  check(entry(graph, Id::Input) == 0, "enabled entry lost its position");
}

void testDisablingKeepsTransitiveOrder() {
  // C depends on A only through B. Dropping B must not let C run before A.
  Graph graph;
  add(graph, Id::C, {Id::B});
  add(graph, Id::B, {Id::A});
  add(graph, Id::A);
  graph.setEnabled(Id::B, false);
  check(activeIds(graph) == std::vector<Id>{Id::A, Id::C},
        "disabled pass dropped its ordering constraint");
  graph.setEnabled(Id::B, true);
  check(activeIds(graph) == std::vector<Id>{Id::A, Id::B, Id::C},
        "re-enabling did not restore the full order");
}
}  // namespace

int main() {
  testRegistrationOrderIsNotExecutionOrder();
  testEnabledEntryKeepsItsPosition();
  testDisabledEntryStartsAtNextEnabledPass();
  testConsecutiveDisabledEntries();
  testNoRemainingEnabledPass();
  testDisablingKeepsTransitiveOrder();
  std::cout << "Audio graph order and entry checks passed\n";
}
