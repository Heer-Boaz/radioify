#include <ggml-backend.h>
#include <ggml-vulkan.h>
#include <whisper.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

void discardLog(ggml_log_level, const char *, void *) {}

int run(int argc, wchar_t **argv) {
  if (argc < 4 || argc > 6) {
    std::cerr << "Usage: whisper_recognition_probe <model> <mono-16k.f32> "
                 "<result.json> [greedy|beam] [no-context]\n";
    return 2;
  }
  const bool beam = argc >= 5 && std::wstring(argv[4]) == L"beam";
  if ((argc >= 5 && !beam && std::wstring(argv[4]) != L"greedy") ||
      (argc == 6 && std::wstring(argv[5]) != L"no-context")) {
    std::cerr << "Invalid recognition strategy or context option.\n";
    return 2;
  }
  const std::filesystem::path outputPath(argv[3]);
  if (std::filesystem::exists(outputPath)) {
    std::cerr << "The diagnostic output already exists.\n";
    return 2;
  }
  std::ifstream audio(std::filesystem::path(argv[2]),
                      std::ios::binary | std::ios::ate);
  if (!audio) {
    std::cerr << "Could not open PCM input.\n";
    return 2;
  }
  const auto bytes = audio.tellg();
  constexpr std::size_t maximumSamples = 60 * WHISPER_SAMPLE_RATE;
  if (bytes <= 0 || bytes > static_cast<std::streamoff>(maximumSamples * sizeof(float)) ||
      bytes % sizeof(float) != 0) {
    std::cerr << "Expected at most 60 seconds of mono float32 PCM at 16 kHz.\n";
    return 2;
  }
  std::vector<float> samples(static_cast<std::size_t>(bytes) / sizeof(float));
  audio.seekg(0);
  if (!audio.read(reinterpret_cast<char *>(samples.data()), bytes) ||
      !std::all_of(samples.begin(), samples.end(),
                   [](float sample) { return std::isfinite(sample); })) {
    std::cerr << "Invalid PCM input.\n";
    return 2;
  }

  whisper_log_set(discardLog, nullptr);
  ggml_log_set(discardLog, nullptr);
  const auto vulkan = ggml_backend_reg_by_name(GGML_VK_NAME);
  int gpuIndex = -1;
  int filteredGpuIndex = 0;
  for (std::size_t index = 0; index < ggml_backend_dev_count(); ++index) {
    const auto device = ggml_backend_dev_get(index);
    const auto type = ggml_backend_dev_type(device);
    if (type != GGML_BACKEND_DEVICE_TYPE_GPU &&
        type != GGML_BACKEND_DEVICE_TYPE_IGPU)
      continue;
    const int currentIndex = filteredGpuIndex++;
    if (ggml_backend_dev_backend_reg(device) != vulkan)
      continue;
    if (gpuIndex < 0 || type == GGML_BACKEND_DEVICE_TYPE_GPU)
      gpuIndex = currentIndex;
    if (type == GGML_BACKEND_DEVICE_TYPE_GPU)
      break;
  }
  if (gpuIndex < 0) {
    std::cerr << "No Vulkan GPU is available.\n";
    return 1;
  }
  auto contextParameters = whisper_context_default_params();
  contextParameters.use_gpu = true;
  contextParameters.require_gpu = true;
  contextParameters.gpu_device = gpuIndex;
  contextParameters.flash_attn = false;
  // This probe intentionally measures the packaged large-v3 recognition path.
  contextParameters.dtw_token_timestamps = true;
  contextParameters.dtw_aheads_preset = WHISPER_AHEADS_LARGE_V3;
  const std::string modelPath = std::filesystem::path(argv[1]).u8string();
  std::unique_ptr<whisper_context, decltype(&whisper_free)> context(
      whisper_init_from_file_with_params(modelPath.c_str(), contextParameters),
      whisper_free);
  if (!context) {
    std::cerr << "Could not load large-v3 on Vulkan.\n";
    return 1;
  }

  auto parameters = whisper_full_default_params(
      beam ? WHISPER_SAMPLING_BEAM_SEARCH : WHISPER_SAMPLING_GREEDY);
  parameters.n_threads = static_cast<int>(
      std::clamp(std::thread::hardware_concurrency(), 1u, 8u));
  parameters.language = "en";
  parameters.suppress_nst = true;
  parameters.token_timestamps = true;
  parameters.thold_pt = 0.01f;
  parameters.print_progress = false;
  parameters.print_realtime = false;
  parameters.print_timestamps = false;
  parameters.greedy.best_of = 5;
  if (argc == 6)
    parameters.n_max_text_ctx = 0;
  const auto begin = std::chrono::steady_clock::now();
  if (whisper_full(context.get(), parameters, samples.data(),
                    static_cast<int>(samples.size())) != 0) {
    std::cerr << "Whisper recognition failed.\n";
    return 1;
  }

  auto segments = nlohmann::json::array();
  for (int index = 0; index < whisper_full_n_segments(context.get()); ++index) {
    auto tokens = nlohmann::json::array();
    for (int tokenIndex = 0;
         tokenIndex < whisper_full_n_tokens(context.get(), index); ++tokenIndex) {
      const auto token = whisper_full_get_token_data(context.get(), index, tokenIndex);
      if (token.id >= whisper_token_eot(context.get()))
        continue;
      tokens.push_back({
          {"text", whisper_full_get_token_text(context.get(), index, tokenIndex)},
          {"probability", token.p}, {"start_us", token.t0 * 10000},
          {"end_us", token.t1 * 10000}, {"alignment_us", token.t_dtw * 10000}});
    }
    const float noSpeech = whisper_full_get_segment_no_speech_prob(context.get(), index);
    segments.push_back({
        {"start_us", whisper_full_get_segment_t0(context.get(), index) * 10000},
        {"end_us", whisper_full_get_segment_t1(context.get(), index) * 10000},
        {"text", whisper_full_get_segment_text(context.get(), index)},
        {"no_speech_probability", noSpeech},
        {"radioify_would_discard", noSpeech >= parameters.no_speech_thold},
        {"tokens", std::move(tokens)}});
  }
  const double elapsed = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - begin).count();
  const nlohmann::json result = {
      {"strategy", beam ? "beam" : "greedy"},
      {"text_context", parameters.n_max_text_ctx}, {"inference_seconds", elapsed},
      {"segments", std::move(segments)}};
  std::ofstream output(outputPath);
  output << result.dump(2) << '\n';
  output.close();
  if (!output) {
    std::cerr << "Could not write diagnostic output.\n";
    return 1;
  }
  std::cout << "Recognized " << result["segments"].size()
            << " raw segments in " << elapsed << " seconds.\n";
  return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) { return run(argc, argv); }
