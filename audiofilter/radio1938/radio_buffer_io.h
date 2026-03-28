#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_BUFFER_IO_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_BUFFER_IO_H

#include <cstddef>
#include <cstdint>

inline float sampleInterleavedToMono(const float* samples,
                                     uint32_t frame,
                                     int channels) {
  if (!samples) return 0.0f;
  if (channels <= 1) return samples[frame];
  float sum = 0.0f;
  const size_t base =
      static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    sum += samples[base + static_cast<size_t>(channel)];
  }
  return sum / static_cast<float>(channels);
}

inline void writeMonoToInterleaved(float* samples,
                                   uint32_t frame,
                                   int channels,
                                   float y) {
  if (!samples) return;
  if (channels <= 1) {
    samples[frame] = y;
    return;
  }
  const size_t base =
      static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    samples[base + static_cast<size_t>(channel)] = y;
  }
}

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO_BUFFER_IO_H
