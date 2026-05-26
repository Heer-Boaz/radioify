#include "audition_worker.h"

#include "audioplayback_internal.h"

#include "audioplayback.h"

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace {

float renderAuditionSample(AuditionTone& tone) {
  if (tone.kind == AuditionKind::Psg) {
    if (!tone.psg) return 0.0f;
    int16_t sample = PSG_calc(tone.psg);
    return (static_cast<float>(sample) / 32768.0f) * tone.gain;
  }
  if (!tone.scc) return 0.0f;
  int16_t sample = SCC_calc(tone.scc);
  return (static_cast<float>(sample) / 32768.0f) * tone.gain;
}

}  // namespace

void stopAuditionWorker() {
  if (!gAudio.audition.active.load()) return;
  gAudio.audition.stop.store(true);
  if (gAudio.audition.worker.joinable()) {
    gAudio.audition.worker.join();
  }
  gAudio.audition.stop.store(false);
  gAudio.audition.active.store(false);
  gAudio.audition.device = KssInstrumentDevice::None;
  gAudio.audition.hash = 0;
}

void startAuditionWorker(AuditionTone tone) {
  gAudio.audition.stop.store(false);
  gAudio.audition.active.store(true);
  gAudio.audition.worker = std::thread([tone = std::move(tone)]() mutable {
    const uint32_t sampleRate = gAudio.sampleRate;
    const uint32_t channels = gAudio.channels;
    constexpr uint64_t kChunkFrames = 512;
    std::vector<float> buffer;
    buffer.resize(static_cast<size_t>(kChunkFrames) * channels);
    uint64_t framePos = 0;
    while (!gAudio.audition.stop.load()) {
      if (!gAudio.state.externalStream.load() ||
          !gAudio.state.streamQueueEnabled.load()) {
        break;
      }
      for (uint64_t i = 0; i < kChunkFrames; ++i) {
        float sample = renderAuditionSample(tone);
        for (uint32_t ch = 0; ch < channels; ++ch) {
          buffer[static_cast<size_t>(i * channels + ch)] = sample;
        }
      }
      uint64_t remaining = kChunkFrames;
      uint64_t offset = 0;
      while (remaining > 0 && !gAudio.audition.stop.load()) {
        int64_t ptsUs =
            static_cast<int64_t>((framePos + offset) * 1000000ULL / sampleRate);
        uint64_t written = 0;
        if (!audioStreamWriteSamples(
                buffer.data() + static_cast<size_t>(offset) * channels,
                remaining, ptsUs, 0, false, &written)) {
          remaining = 0;
          break;
        }
        if (written == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        remaining -= written;
        offset += written;
      }
      framePos += offset;
    }
    if (tone.kind == AuditionKind::Psg && tone.psg) {
      PSG_delete(tone.psg);
      tone.psg = nullptr;
    }
    if (tone.kind == AuditionKind::SccWave && tone.scc) {
      SCC_delete(tone.scc);
      tone.scc = nullptr;
    }
    gAudio.audition.active.store(false);
  });
}
