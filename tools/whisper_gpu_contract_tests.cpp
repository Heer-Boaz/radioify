#include <whisper.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>

namespace {

struct WhisperContextDeleter {
  void operator()(whisper_context* context) const {
    if (context) whisper_free(context);
  }
};

using WhisperContextPtr =
    std::unique_ptr<whisper_context, WhisperContextDeleter>;

whisper_context* loadModel(const std::filesystem::path& modelPath,
                           bool requireGpu) {
  whisper_context_params parameters = whisper_context_default_params();
  parameters.use_gpu = false;
  parameters.require_gpu = requireGpu;
  parameters.flash_attn = false;
  const std::string modelPathUtf8 = modelPath.u8string();
  return whisper_init_from_file_with_params(modelPathUtf8.c_str(), parameters);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: whisper_gpu_contract_tests <model-path>\n";
    return EXIT_FAILURE;
  }

  const std::filesystem::path modelPath(argv[1]);
  if (!std::filesystem::is_regular_file(modelPath)) {
    std::cerr << "whisper_gpu_contract_tests: model does not exist: "
              << modelPath.string() << '\n';
    return EXIT_FAILURE;
  }

  WhisperContextPtr cpuAllowed(loadModel(modelPath, false));
  if (!cpuAllowed) {
    std::cerr << "whisper_gpu_contract_tests: control initialization failed\n";
    return EXIT_FAILURE;
  }
  if (!whisper_is_multilingual(cpuAllowed.get())) {
    std::cerr << "whisper_gpu_contract_tests: packaged model cannot translate "
                 "speech to English\n";
    return EXIT_FAILURE;
  }

  WhisperContextPtr gpuRequired(loadModel(modelPath, true));
  if (gpuRequired) {
    std::cerr << "whisper_gpu_contract_tests: require_gpu silently fell back "
                 "to CPU\n";
    return EXIT_FAILURE;
  }

  std::cout << "whisper_gpu_contract_tests: PASS\n";
  return EXIT_SUCCESS;
}
