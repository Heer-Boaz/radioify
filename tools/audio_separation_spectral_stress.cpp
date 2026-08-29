#include "audio/separation/artifact.h"
#include "audio/separation/spectral_transform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <numbers>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kChannels = 2;
constexpr double kSampleRate = 48000.0;

int parseIterations(const wchar_t* value) {
  if (!value) return 0;
  wchar_t* end = nullptr;
  const long parsed = std::wcstol(value, &end, 10);
  if (!end || *end != L'\0' || parsed <= 0 || parsed > 100000) return 0;
  return static_cast<int>(parsed);
}

bool processStereoChunk(audio_separation::BanditSpectralTransform& transform,
                        const std::vector<float>& input,
                        std::vector<float>* spectrogram,
                        std::vector<float>* identityMask,
                        std::vector<float>* reconstructed, std::string* error) {
  for (std::size_t channel = 0; channel < kChannels; ++channel) {
    if (!transform.forward(input.data(), input.size(), spectrogram, error)) {
      return false;
    }
    if (identityMask->size() != spectrogram->size()) {
      identityMask->assign(spectrogram->size(), 0.0f);
      for (std::size_t index = 0; index < identityMask->size(); index += 2) {
        (*identityMask)[index] = 1.0f;
      }
    }
    for (std::size_t stem = 0; stem < audio_separation::kStemCount; ++stem) {
      if (!transform.inverseMasked(*spectrogram, identityMask->data(),
                                   reconstructed, error)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc > 2) {
    std::cerr << "Usage: audio_separation_spectral_stress [iterations]\n";
    return 2;
  }
  const int iterations = argc == 2 ? parseIterations(argv[1]) : 32;
  if (iterations <= 0) {
    std::cerr << "Iterations must be between 1 and 100000.\n";
    return 2;
  }

  audio_separation::BanditSpectralTransform transform;
  std::string error;
  if (!transform.initialize(&error)) {
    std::cerr << "Spectral transform initialization failed: " << error << '\n';
    return EXIT_FAILURE;
  }

  std::vector<float> input(
      audio_separation::BanditSpectralTransform::kChunkFrames);
  for (std::size_t frame = 0; frame < input.size(); ++frame) {
    const double time = static_cast<double>(frame) / kSampleRate;
    input[frame] = static_cast<float>(
        0.2 * std::sin(2.0 * std::numbers::pi * 220.0 * time) +
        0.1 * std::sin(2.0 * std::numbers::pi * 997.0 * time));
  }

  std::vector<float> spectrogram;
  std::vector<float> identityMask;
  std::vector<float> reconstructed;
  if (!processStereoChunk(transform, input, &spectrogram, &identityMask,
                          &reconstructed, &error)) {
    std::cerr << "Warm-up failed: " << error << '\n';
    return EXIT_FAILURE;
  }

  const auto started = std::chrono::steady_clock::now();
  for (int iteration = 0; iteration < iterations; ++iteration) {
    if (!processStereoChunk(transform, input, &spectrogram, &identityMask,
                            &reconstructed, &error)) {
      std::cerr << "Iteration " << (iteration + 1) << " failed: " << error
                << '\n';
      return EXIT_FAILURE;
    }
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();

  float maximumError = 0.0f;
  for (std::size_t frame = 0; frame < input.size(); ++frame) {
    maximumError =
        std::max(maximumError, std::abs(input[frame] - reconstructed[frame]));
  }
  if (maximumError >= 2.0e-4f) {
    std::cerr << "Identity round-trip exceeded the spectral contract: "
              << maximumError << '\n';
    return EXIT_FAILURE;
  }

  std::cout << iterations << " stereo chunks in " << elapsed << " s\n"
            << "Per chunk: " << (elapsed / iterations) << " s\n"
            << "Transforms per chunk: " << kChannels << " STFT + "
            << (kChannels * audio_separation::kStemCount) << " ISTFT\n"
            << "Identity maximum error: " << maximumError << '\n';
  return EXIT_SUCCESS;
}
