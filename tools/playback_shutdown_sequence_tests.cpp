#include "playback/session/shutdown_sequence.h"

#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "playback_shutdown_sequence_tests: " << message << '\n';
  return false;
}

struct ParticipantStub {
  bool ready = false;
  int requestCount = 0;
  int finishCount = 0;
  std::vector<NativeWaitHandle> handles;

  playback_session::ShutdownSequence::Participant bind() {
    return {[this]() { ++requestCount; }, [this]() { return ready; },
            [this]() {
              ++finishCount;
              return ready;
            },
            [this]() { return handles; }};
  }
};

}  // namespace

int main() {
  bool ok = true;
  ParticipantStub preview;
  ParticipantStub editor;
  ParticipantStub window;
  ParticipantStub player;
  preview.handles = {NativeWaitHandle(reinterpret_cast<void*>(1))};
  editor.handles = {NativeWaitHandle(reinterpret_cast<void*>(2)),
                    NativeWaitHandle(reinterpret_cast<void*>(3))};
  window.handles = {NativeWaitHandle(reinterpret_cast<void*>(4))};
  player.handles = {NativeWaitHandle(reinterpret_cast<void*>(5))};

  playback_session::ShutdownSequence sequence(
      {preview.bind(), editor.bind(), window.bind(), player.bind()});
  ok &= expect(!sequence.requested() && !sequence.ready() &&
                   !sequence.finished() && sequence.waitHandles().empty(),
               "an idle sequence must not expose shutdown readiness");

  ok &= expect(sequence.requestStop() && !sequence.requestStop() &&
                   preview.requestCount == 1 && editor.requestCount == 1 &&
                   window.requestCount == 1 && player.requestCount == 1,
               "shutdown must be requested exactly once from every owner");
  ok &= expect(sequence.waitHandles().size() == 5,
               "the shutdown wait set must include every participant handle");
  ok &= expect(!sequence.ready() && !sequence.finish() &&
                   preview.finishCount == 0 && editor.finishCount == 0 &&
                   window.finishCount == 0 && player.finishCount == 0,
               "no participant may be finalized before the full barrier");

  preview.ready = true;
  editor.ready = true;
  window.ready = true;
  ok &= expect(sequence.waitHandles().size() == 1,
               "ready participants must leave the active wait set");
  ok &= expect(!sequence.ready() && !sequence.finish(),
               "one running participant must hold the entire barrier");
  player.ready = true;
  ok &= expect(sequence.ready() && sequence.finish() && sequence.finished() &&
                   preview.finishCount == 1 && editor.finishCount == 1 &&
                   window.finishCount == 1 && player.finishCount == 1 &&
                   sequence.waitHandles().empty(),
               "all participants must finalize once after all become ready");
  ok &= expect(sequence.finish() && preview.finishCount == 1 &&
                   editor.finishCount == 1 && window.finishCount == 1 &&
                   player.finishCount == 1,
               "finished shutdown must be idempotent");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
