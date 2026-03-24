#include "loopsplit.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ffmpegaudio.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "kssaudio.h"
#include "m4adecoder.h"
#include "midiaudio.h"
#include "psfaudio.h"
#include "vgmaudio.h"

#define MA_ENABLE_WAV
#define MA_ENABLE_MP3
#define MA_ENABLE_FLAC
#include "miniaudio.h"

namespace {

constexpr float kScoreEpsilon = 1e-6f;

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4" || ext == ".mov" || ext == ".ogg" ||
         ext == ".kss" ||
         ext == ".nsf" ||
#if !RADIOIFY_DISABLE_GSF_GPL
         ext == ".gsf" || ext == ".minigsf" ||
#endif
         ext == ".mid" || ext == ".midi" ||
         ext == ".vgm" || ext == ".vgz" || ext == ".psf" ||
         ext == ".minipsf" ||
         ext == ".psf2" || ext == ".minipsf2";
}

bool isM4aExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm" || ext == ".mov";
}

bool isMiniaudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

bool isGmeExt(const std::filesystem::path& p) {
  return toLower(p.extension().string()) == ".nsf";
}

bool isPsfExt(const std::filesystem::path& p) {
  const std::string ext = toLower(p.extension().string());
  return ext == ".psf" || ext == ".minipsf" || ext == ".psf2" ||
         ext == ".minipsf2";
}

bool isMidiExt(const std::filesystem::path& p) {
  const std::string ext = toLower(p.extension().string());
  return ext == ".mid" || ext == ".midi";
}

bool isGsfExt(const std::filesystem::path& p) {
#if !RADIOIFY_DISABLE_GSF_GPL
  const std::string ext = toLower(p.extension().string());
  return ext == ".gsf" || ext == ".minigsf";
#else
  (void)p;
  return false;
#endif
}

bool isVgmExt(const std::filesystem::path& p) {
  const std::string ext = toLower(p.extension().string());
  return ext == ".vgm" || ext == ".vgz";
}

bool isKssExt(const std::filesystem::path& p) {
  return toLower(p.extension().string()) == ".kss";
}

uint64_t secToFrames(double seconds, uint32_t sampleRate) {
  if (seconds <= 0.0 || sampleRate == 0) return 0;
  return static_cast<uint64_t>(std::llround(seconds * static_cast<double>(sampleRate)));
}

size_t secToFeatureFrames(double seconds, double featureRate, size_t maxFrames) {
  if (seconds <= 0.0 || featureRate <= 0.0) return 0;
  const auto frames = static_cast<int64_t>(std::llround(seconds * featureRate));
  if (frames <= 0) return 0;
  return static_cast<size_t>(std::clamp<int64_t>(frames, 1, static_cast<int64_t>(maxFrames)));
}

struct MatchCandidate {
  size_t start = 0;
  size_t len = 0;
  float score = -1.0f;
  bool valid = false;
};

class LoopSplitDecoder {
 public:
  enum class Type {
    Unknown,
    Ffmpeg,
    M4a,
    Gme,
    Midi,
    Gsf,
    Vgm,
    Kss,
    Psf,
    Miniaudio
  };

  bool init(const std::filesystem::path& file, uint32_t channels,
            uint32_t sampleRate, const LoopSplitConfig& config,
            std::string* error) {
    close();

    const bool useM4a = isM4aExt(file);
    const bool useMiniaudio = isMiniaudioExt(file);
    const bool useGme = isGmeExt(file);
    const bool useMidi = isMidiExt(file);
    const bool useGsf = isGsfExt(file);
    const bool useVgm = isVgmExt(file);
    const bool useKss = isKssExt(file);
    const bool usePsf = isPsfExt(file);
    if (!useM4a && !useMiniaudio && !useGme && !useMidi && !useGsf &&
        !useVgm && !useKss && !usePsf) {
      if (error) {
        *error = "Unsupported input format for loop split analysis.";
      }
      return false;
    }

    if (useM4a) {
      if (!m4a_.init(file, channels, sampleRate, error)) {
        close();
        return false;
      }
      type_ = Type::M4a;
      return true;
    }

    if (useKss) {
      if (!kss_.init(file, channels, sampleRate, error, config.trackIndex,
                     config.kssOptions)) {
        close();
        return false;
      }
      type_ = Type::Kss;
      return true;
    }

    if (usePsf) {
      if (!psf_.init(file, channels, sampleRate, error, config.trackIndex)) {
        close();
        return false;
      }
      type_ = Type::Psf;
      return true;
    }

    if (useGsf) {
      if (!gsf_.init(file, channels, sampleRate, error, config.trackIndex)) {
        close();
        return false;
      }
      type_ = Type::Gsf;
      return true;
    }

    if (useVgm) {
      if (!vgm_.init(file, channels, sampleRate, error)) {
        close();
        return false;
      }
      vgm_.applyOptions(config.vgmOptions);
      type_ = Type::Vgm;
      return true;
    }

    if (useGme) {
      if (!gme_.init(file, channels, sampleRate, error, config.trackIndex)) {
        close();
        return false;
      }
      gme_.applyNsfOptions(config.nsfOptions);
      type_ = Type::Gme;
      return true;
    }

    if (useMidi) {
      if (!midi_.init(file, channels, sampleRate, error)) {
        close();
        return false;
      }
      type_ = Type::Midi;
      return true;
    }

    if (useMiniaudio) {
      ma_decoder_config decConfig =
          ma_decoder_config_init(ma_format_f32, channels, sampleRate);
      if (ma_decoder_init_file(file.string().c_str(), &decConfig, &decoder_) !=
          MA_SUCCESS) {
        close();
        if (error) {
          *error = "Failed to open input for decoding.";
        }
        return false;
      }
      type_ = Type::Miniaudio;
      return true;
    }

    if (useFfmpeg(file)) {
      if (!ffmpeg_.init(file, channels, sampleRate, error)) {
        close();
        return false;
      }
      type_ = Type::Ffmpeg;
      return true;
    }

    if (error) {
      *error = "Unsupported format for loop split analysis.";
    }
    close();
    return false;
  }

  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead) {
    if (framesRead) *framesRead = 0;
    if (!out || frameCount == 0 || type_ == Type::Unknown) {
      return false;
    }
    switch (type_) {
      case Type::Ffmpeg:
        return ffmpeg_.readFrames(out, frameCount, framesRead);
      case Type::M4a:
        return m4a_.readFrames(out, frameCount, framesRead);
      case Type::Gme:
        return gme_.readFrames(out, frameCount, framesRead);
      case Type::Midi:
        return midi_.readFrames(out, frameCount, framesRead);
      case Type::Gsf:
        return gsf_.readFrames(out, frameCount, framesRead);
      case Type::Vgm:
        return vgm_.readFrames(out, frameCount, framesRead);
      case Type::Kss:
        return kss_.readFrames(out, frameCount, framesRead);
      case Type::Psf:
        return psf_.readFrames(out, frameCount, framesRead);
      case Type::Miniaudio: {
        ma_uint64 read = 0;
        ma_result res = ma_decoder_read_pcm_frames(&decoder_, out, frameCount, &read);
        if (framesRead) *framesRead = static_cast<uint64_t>(read);
        if (res == MA_SUCCESS || res == MA_AT_END) {
          return true;
        }
        return false;
      }
      default:
        return false;
    }
  }

  bool getTotalFrames(uint64_t* outFrames) {
    if (!outFrames) return false;
    *outFrames = 0;
    switch (type_) {
      case Type::Ffmpeg:
        return ffmpeg_.getTotalFrames(outFrames);
      case Type::M4a:
        return m4a_.getTotalFrames(outFrames);
      case Type::Gme:
        return gme_.getTotalFrames(outFrames);
      case Type::Midi:
        return midi_.getTotalFrames(outFrames);
      case Type::Gsf:
        return gsf_.getTotalFrames(outFrames);
      case Type::Vgm:
        return vgm_.getTotalFrames(outFrames);
      case Type::Kss:
        return kss_.getTotalFrames(outFrames);
      case Type::Psf:
        return psf_.getTotalFrames(outFrames);
      case Type::Miniaudio: {
        ma_uint64 total = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder_, &total) != MA_SUCCESS) {
          return false;
        }
        *outFrames = static_cast<uint64_t>(total);
        return true;
      }
      default:
        return false;
    }
  }

  void close() {
    if (type_ == Type::Ffmpeg) {
      ffmpeg_.uninit();
    } else if (type_ == Type::M4a) {
      m4a_.uninit();
    } else if (type_ == Type::Gme) {
      gme_.uninit();
    } else if (type_ == Type::Midi) {
      midi_.uninit();
    } else if (type_ == Type::Gsf) {
      gsf_.uninit();
    } else if (type_ == Type::Vgm) {
      vgm_.uninit();
    } else if (type_ == Type::Kss) {
      kss_.uninit();
    } else if (type_ == Type::Psf) {
      psf_.uninit();
    } else if (type_ == Type::Miniaudio) {
      ma_decoder_uninit(&decoder_);
    }
    type_ = Type::Unknown;
  }

 ~LoopSplitDecoder() { close(); }

 private:
  bool useFfmpeg(const std::filesystem::path& file) const {
    return isSupportedAudioExt(file) && !isM4aExt(file) && !isMiniaudioExt(file) &&
           !isGmeExt(file) && !isMidiExt(file) && !isGsfExt(file) &&
           !isVgmExt(file) && !isKssExt(file) && !isPsfExt(file);
  }

  Type type_ = Type::Unknown;
  FfmpegAudioDecoder ffmpeg_;
  M4aDecoder m4a_;
  GmeAudioDecoder gme_;
  GsfAudioDecoder gsf_;
  MidiAudioDecoder midi_;
  VgmAudioDecoder vgm_;
  KssAudioDecoder kss_;
  PsfAudioDecoder psf_;
  ma_decoder decoder_{};
};

float featureCompare(const std::vector<float>& feature, size_t aStart,
                    size_t bStart, size_t frameCount) {
  if (frameCount == 0) return -1.0f;
  if (aStart + frameCount > feature.size()) return -1.0f;
  if (bStart + frameCount > feature.size()) return -1.0f;
  double err = 0.0;
  double norm = 0.0;
  for (size_t i = 0; i < frameCount; ++i) {
    const double a = feature[aStart + i];
    const double b = feature[bStart + i];
    const double d = a - b;
    err += d * d;
    norm += a * a + b * b;
  }
  if (norm <= kScoreEpsilon) {
    return 0.0f;
  }
  return static_cast<float>(std::clamp(1.0 - (err / (norm + kScoreEpsilon)),
                                     -1.0, 1.0));
}

MatchCandidate bestMatchAtLength(const std::vector<float>& feature, size_t len,
                                size_t window) {
  MatchCandidate best;
  if (feature.empty() || len == 0) return best;
  window = std::min(window, len);
  if (window == 0) return best;
  const size_t maxStart = feature.size() >= len + window
                             ? feature.size() - len - window
                             : 0;
  if (maxStart == 0) {
    float score = featureCompare(feature, 0, len, window);
    if (score > best.score) {
      best.start = 0;
      best.len = len;
      best.score = score;
      best.valid = true;
    }
    return best;
  }

  double err = 0.0;
  double normA = 0.0;
  double normB = 0.0;
  for (size_t i = 0; i < window; ++i) {
    const double a = feature[i];
    const double b = feature[i + len];
    const double d = a - b;
    err += d * d;
    normA += a * a;
    normB += b * b;
  }
  auto scoreFrom = [&](double e, double a2, double b2) -> float {
    const double norm = a2 + b2;
    if (norm <= kScoreEpsilon) return 0.0f;
    return static_cast<float>(std::clamp(1.0 - (e / (norm + kScoreEpsilon)),
                                       -1.0, 1.0));
  };
  float bestScore = scoreFrom(err, normA, normB);
  size_t bestStart = 0;

  for (size_t start = 1; start <= maxStart; ++start) {
    const size_t outA = start - 1;
    const size_t outB = outA + len;
    const double outDA = feature[outA];
    const double outDB = feature[outB];
    err -= (outDA - outDB) * (outDA - outDB);
    normA -= outDA * outDA;
    normB -= outDB * outDB;

    const size_t inA = start + window - 1;
    const size_t inB = inA + len;
    const double inDA = feature[inA];
    const double inDB = feature[inB];
    const double inDiff = inDA - inDB;
    err += inDiff * inDiff;
    normA += inDA * inDA;
    normB += inDB * inDB;

    const float score = scoreFrom(err, normA, normB);
    if (score > bestScore) {
      bestScore = score;
      bestStart = start;
    }
  }

  best.start = bestStart;
  best.len = len;
  best.score = bestScore;
  best.valid = true;
  return best;
}

std::vector<float> buildFeature(const std::vector<float>& mono,
                               uint32_t hopFrames) {
  std::vector<float> feature;
  if (mono.empty() || hopFrames == 0) return feature;
  const size_t frames = mono.size();
  const size_t blockCount = (frames + hopFrames - 1) / hopFrames;
  feature.reserve(blockCount);

  for (size_t block = 0; block < blockCount; ++block) {
    const size_t base = block * hopFrames;
    const size_t end = std::min<size_t>(frames, base + hopFrames);
    const size_t n = end - base;
    double sumSq = 0.0;
    for (size_t i = base; i < end; ++i) {
      const double v = mono[i];
      sumSq += v * v;
    }
    const float value =
        static_cast<float>(std::sqrt(sumSq / static_cast<double>(n)));
    feature.push_back(std::isfinite(value) ? value : 0.0f);
  }
  return feature;
}

std::vector<float> toMono(const std::vector<float>& interleaved,
                         uint32_t channels) {
  if (channels == 0) return {};
  const size_t frameCount = interleaved.size() / channels;
  std::vector<float> mono(frameCount, 0.0f);
  if (channels == 1) {
    for (size_t i = 0; i < frameCount; ++i) {
      mono[i] = interleaved[i];
    }
    return mono;
  }

  const float invCh = 1.0f / static_cast<float>(channels);
  for (size_t i = 0; i < frameCount; ++i) {
    float sum = 0.0f;
    const size_t base = i * channels;
    for (uint32_t c = 0; c < channels; ++c) {
      sum += interleaved[base + c];
    }
    mono[i] = sum * invCh;
  }
  return mono;
}

MatchCandidate detectLoopByFeatures(const std::vector<float>& feature,
                                   double featureRate,
                                   const LoopSplitConfig& config) {
  MatchCandidate fallback{};
  if (feature.empty()) return fallback;
  const size_t maxLen = feature.size() / 2;
  if (maxLen < 2) return fallback;

  const size_t minLen = std::min(
      maxLen, std::max<size_t>(1u, secToFeatureFrames(config.minLoopSeconds,
                                                      featureRate, maxLen)));
  size_t maxLoopLen =
      std::min(maxLen, secToFeatureFrames(config.maxLoopSeconds, featureRate, maxLen));
  if (maxLoopLen < minLen) {
    maxLoopLen = minLen;
  }

  const size_t coarseWindow =
      std::max<size_t>(1u, secToFeatureFrames(config.coarseWindowSeconds,
                                              featureRate, maxLen));
  const size_t refineWindow =
      std::max<size_t>(1u, secToFeatureFrames(config.refineWindowSeconds,
                                              featureRate, maxLen));
  const size_t lenStep =
      std::max<size_t>(1u, secToFeatureFrames(0.1, featureRate, maxLen));
  const size_t startScanStep =
      std::max<size_t>(1u, secToFeatureFrames(0.5, featureRate, maxLen));
  const size_t maxStartScan =
      std::min<size_t>(feature.size() / 2, secToFeatureFrames(60.0, featureRate, maxLen));

  std::vector<size_t> coarseStarts;
  for (size_t s = 0; s <= maxStartScan; s += startScanStep) {
    coarseStarts.push_back(s);
  }
  if (coarseStarts.empty() || coarseStarts.back() != maxStartScan) {
    coarseStarts.push_back(maxStartScan);
  }

  struct LenScore {
    size_t len;
    float score;
  };
  std::vector<LenScore> scores;

  for (size_t len = minLen; len <= maxLoopLen; len += lenStep) {
    const size_t window = std::min(coarseWindow, len);
    float bestLenScore = -1.0f;
    for (size_t start : coarseStarts) {
      if (start + len + window > feature.size()) continue;
      const float score = featureCompare(feature, start, start + len, window);
      if (score > bestLenScore) {
        bestLenScore = score;
      }
    }
    if (bestLenScore > -1.0f) {
      scores.push_back({len, bestLenScore});
    }
  }
  if (scores.empty()) return fallback;

  std::sort(scores.begin(), scores.end(),
            [](const LenScore& a, const LenScore& b) {
              return a.score > b.score;
            });
  if (scores.size() > 12) scores.resize(12);

  std::vector<size_t> candidateLens;
  for (const auto& entry : scores) {
    candidateLens.push_back(entry.len);
    candidateLens.push_back(entry.len > 0 ? entry.len - 1 : 0);
    candidateLens.push_back(entry.len + 1);
    candidateLens.push_back(entry.len > lenStep ? entry.len - lenStep : entry.len);
    candidateLens.push_back(entry.len + lenStep);
  }
  std::sort(candidateLens.begin(), candidateLens.end());
  candidateLens.erase(std::unique(candidateLens.begin(), candidateLens.end()),
                      candidateLens.end());

  MatchCandidate best{};
  for (size_t len : candidateLens) {
    if (len < minLen || len > maxLoopLen) continue;
    if (len == 0) continue;
    if (len + 1 >= feature.size()) continue;
    MatchCandidate candidate = bestMatchAtLength(feature, len, refineWindow);
    if (!candidate.valid) continue;
    if (!best.valid || candidate.score > best.score) {
      best = candidate;
    }
  }
  return best;
}

MatchCandidate refineSampleMatch(const std::vector<float>& mono,
                                size_t sampleRate, uint64_t approxStartFrame,
                                uint64_t approxLenFrames,
                                const LoopSplitConfig& config,
                                size_t minLoopFrames,
                                size_t maxLoopFrames) {
  MatchCandidate best;
  if (mono.empty() || approxLenFrames == 0 || sampleRate == 0) return best;
  if (approxStartFrame >= mono.size()) return best;

  const size_t shiftLimit = std::max<size_t>(1u, config.analysisHop);
  const int64_t shiftStep = std::max<int64_t>(16, static_cast<int64_t>(config.analysisHop / 8));
  const size_t maxWindow = std::max<size_t>(1u, secToFrames(config.refineWindowSeconds, sampleRate));
  const size_t maxStart = std::min<size_t>(mono.size() - 1, approxStartFrame + shiftLimit);
  const size_t minStart = approxStartFrame > shiftLimit ? approxStartFrame - shiftLimit : 0;

  int64_t bestStart = static_cast<int64_t>(approxStartFrame);
  int64_t bestLen = static_cast<int64_t>(approxLenFrames);
  float bestScore = -1.0f;

  for (int64_t ds = -static_cast<int64_t>(shiftLimit);
       ds <= static_cast<int64_t>(shiftLimit); ds += shiftStep) {
    const int64_t start = static_cast<int64_t>(approxStartFrame) + ds;
    if (start < 0 || static_cast<size_t>(start) >= mono.size()) continue;
    const size_t clampedStart = static_cast<size_t>(start);

    const int64_t startMinLen = std::max<int64_t>(static_cast<int64_t>(minLoopFrames),
                                                  static_cast<int64_t>(1));
    const int64_t startMaxLen =
        static_cast<int64_t>(std::min<size_t>(maxLoopFrames, mono.size() - clampedStart));
    if (startMaxLen <= 0) continue;

    for (int64_t dl = -static_cast<int64_t>(shiftLimit);
         dl <= static_cast<int64_t>(shiftLimit); dl += shiftStep) {
      int64_t len = static_cast<int64_t>(approxLenFrames) + dl;
      if (len < startMinLen || len > startMaxLen) continue;

      size_t end = clampedStart + static_cast<size_t>(len);
      if (end >= mono.size()) continue;
      size_t compareWindow = std::min<size_t>(maxWindow, static_cast<size_t>(len));
      if (compareWindow == 0) continue;

      const float score =
          featureCompare(mono, clampedStart, end, compareWindow);
      if (score > bestScore) {
        bestScore = score;
        bestStart = static_cast<int64_t>(clampedStart);
        bestLen = len;
      }
    }
  }

  if (bestScore < 0.0f || bestStart < 0 || bestLen <= 0) return best;
  best.start = static_cast<size_t>(bestStart);
  best.len = static_cast<size_t>(bestLen);
  best.score = bestScore;
  best.valid = true;
  return best;
}

bool writeSegment(const std::filesystem::path& outputPath,
                 const std::vector<float>& interleaved, uint32_t channels,
                 uint32_t sampleRate, uint64_t startFrame, uint64_t frameCount,
                 std::string* error) {
  if (frameCount == 0) return false;
  const uint64_t totalFrames = interleaved.size() / channels;
  if (channels == 0 || sampleRate == 0 || startFrame >= totalFrames ||
      startFrame + frameCount > totalFrames) {
    if (error) *error = "Invalid segment boundaries for export.";
    return false;
  }

  ma_encoder encoder{};
  ma_encoder_config encConfig =
      ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, channels,
                            sampleRate);
  if (ma_encoder_init_file(outputPath.string().c_str(), &encConfig, &encoder) !=
      MA_SUCCESS) {
    if (error) *error = "Failed to initialize WAV encoder.";
    return false;
  }

  const uint64_t maxChunkFrames = 1024;
  std::vector<float> floatBuffer(static_cast<size_t>(maxChunkFrames) * channels);
  std::vector<int16_t> intBuffer(static_cast<size_t>(maxChunkFrames) * channels);

  bool ok = true;
  uint64_t written = 0;
  while (written < frameCount && ok) {
    const uint64_t framesLeft = frameCount - written;
    const uint64_t framesNow = std::min<uint64_t>(maxChunkFrames, framesLeft);
    size_t sourceBase = static_cast<size_t>((startFrame + written) * channels);
    size_t sampleCount = static_cast<size_t>(framesNow * channels);
    for (size_t i = 0; i < sampleCount; ++i) {
      const float v = std::clamp(
          interleaved[sourceBase + i], -1.0f,
          1.0f);
      intBuffer[i] = static_cast<int16_t>(std::lround(v * 32767.0f));
    }
    ma_uint64 framesWritten = 0;
    if (ma_encoder_write_pcm_frames(&encoder, intBuffer.data(), framesNow,
                                   &framesWritten) != MA_SUCCESS ||
        framesWritten != framesNow) {
      ok = false;
      break;
    }
    written += framesNow;
  }

  ma_encoder_uninit(&encoder);
  if (!ok || written != frameCount) {
    if (error) *error = "Failed to write WAV segment.";
    return false;
  }
  return true;
}

bool decodeToInterleaved(const std::filesystem::path& inputFile,
                        const LoopSplitConfig& config, std::vector<float>& out,
                        uint64_t* totalFrames, std::string* error) {
  if (totalFrames) *totalFrames = 0;
  out.clear();
  LoopSplitDecoder decoder;
  if (!decoder.init(inputFile, config.channels, config.sampleRate, config,
                   error)) {
    return false;
  }

  uint64_t total = 0;
  decoder.getTotalFrames(&total);
  if (totalFrames) *totalFrames = total;
  const uint64_t estimatedFrames = total > 0 ? total : 0;
  if (estimatedFrames > 0) {
    out.reserve(static_cast<size_t>(estimatedFrames) * config.channels);
  }

  const uint32_t chunkFrames = 2048;
  std::vector<float> buffer(
      static_cast<size_t>(chunkFrames) * config.channels);
  uint64_t processed = 0;

  while (true) {
    uint64_t framesRead = 0;
    if (!decoder.readFrames(buffer.data(), chunkFrames, &framesRead)) {
      decoder.close();
      if (error) *error = "Failed to decode source audio.";
      return false;
    }
    if (framesRead == 0) break;

    const size_t sampleCount =
        static_cast<size_t>(framesRead * config.channels);
    out.insert(out.end(), buffer.data(), buffer.data() + sampleCount);
    processed += framesRead;
    if (total > 0 && processed >= total) break;
  }
  decoder.close();

  if (out.empty()) {
    if (error) *error = "No audio frames were decoded.";
    return false;
  }
  if (totalFrames) *totalFrames = processed;
  return true;
}

}  // namespace

bool splitAudioIntoLoopFiles(const std::filesystem::path& inputFile,
                            const std::filesystem::path& stingerOutput,
                            const std::filesystem::path& loopOutput,
                            const LoopSplitConfig& config,
                            LoopSplitResult* result,
                            std::string* error) {
  if (result) {
    *result = LoopSplitResult{};
  }
  if (!result) {
    if (error) *error = "No result object provided.";
    return false;
  }
  if (inputFile.empty()) {
    if (error) *error = "Missing input file path.";
    return false;
  }
  if (config.channels != 1 && config.channels != 2) {
    if (error) *error = "Loop split supports only mono or stereo output channels.";
    return false;
  }
  if (loopOutput.empty()) {
    if (error) *error = "Loop output path is required.";
    return false;
  }
  if (!isSupportedAudioExt(inputFile)) {
    if (error) *error = "Unsupported input format.";
    return false;
  }
  if (!std::filesystem::exists(inputFile)) {
    if (error) *error = "Input file not found.";
    return false;
  }

  uint32_t sampleRate = std::max<uint32_t>(1u, config.sampleRate);
  uint64_t totalFrames = 0;
  std::vector<float> interleaved;
  if (!decodeToInterleaved(inputFile, config, interleaved,
                           &totalFrames, error)) {
    return false;
  }
  if (totalFrames == 0 || interleaved.empty()) {
    if (error) *error = "No audio data decoded.";
    return false;
  }
  result->totalFrames = totalFrames;

  std::vector<float> mono = toMono(interleaved, config.channels);
  if (mono.empty()) {
    if (error) *error = "Failed to build analysis signal.";
    return false;
  }

  const uint64_t minLoopFrames = secToFrames(config.minLoopSeconds, sampleRate);
  const uint64_t maxLoopFrames = secToFrames(config.maxLoopSeconds, sampleRate);
  const double featureRate =
      static_cast<double>(sampleRate) / static_cast<double>(std::max<uint32_t>(1u, config.analysisHop));
  const std::vector<float> feature = buildFeature(mono, config.analysisHop);
  MatchCandidate match{};
  if (feature.size() >= 4) {
    match = detectLoopByFeatures(feature, featureRate, config);
  }

  bool hasMatch = match.valid && match.score >= config.minConfidence;
  if (!hasMatch) {
    match.start = 0;
    match.len = static_cast<size_t>(totalFrames);
    match.score = std::max(0.0f, match.score);
  }

  size_t approxStartFrame = match.start * std::max<uint32_t>(1u, config.analysisHop);
  size_t approxLenFrame = match.len * std::max<uint32_t>(1u, config.analysisHop);
  if (approxLenFrame > totalFrames) {
    approxLenFrame = static_cast<size_t>(totalFrames);
  }

  if (hasMatch) {
    MatchCandidate refined = refineSampleMatch(
        mono, sampleRate, approxStartFrame, approxLenFrame, config,
        static_cast<size_t>(std::min<uint64_t>(totalFrames,
                                               std::max<uint64_t>(1u, minLoopFrames))),
        static_cast<size_t>(std::min<uint64_t>(totalFrames,
                                               std::max<uint64_t>(1u, maxLoopFrames))));
    if (refined.valid) {
      match = refined;
      hasMatch = match.score >= config.minConfidence;
      approxStartFrame = match.start;
      approxLenFrame = match.len;
    }
  }

  approxStartFrame = std::min<size_t>(approxStartFrame, totalFrames);
  if (approxStartFrame > static_cast<size_t>(totalFrames)) approxStartFrame = 0;
  if (approxStartFrame + approxLenFrame > static_cast<size_t>(totalFrames)) {
    approxLenFrame = static_cast<size_t>(totalFrames - approxStartFrame);
  }

  result->loopStartFrame = approxStartFrame;
  result->loopFrameCount = approxLenFrame;
  result->confidence = match.score;
  result->hasLoop = hasMatch && approxLenFrame >= std::max<size_t>(1u, minLoopFrames);
  result->hasStinger =
      result->hasLoop && result->loopStartFrame >= config.analysisHop;
  if (!result->hasLoop) {
    result->loopStartFrame = 0;
    result->loopFrameCount = totalFrames;
    result->hasStinger = false;
  }

  if (result->hasStinger && !stingerOutput.empty() &&
      !writeSegment(stingerOutput, interleaved, config.channels, sampleRate,
                    0, result->loopStartFrame, error)) {
    return false;
  }

  if (result->loopFrameCount == 0) {
    if (error) *error = "No loop segment found.";
    return false;
  }
  if (!writeSegment(loopOutput, interleaved, config.channels, sampleRate,
                   result->loopStartFrame, result->loopFrameCount, error)) {
    return false;
  }
  return true;
}
