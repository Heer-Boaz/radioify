#include "tui_export.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "app_common.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "audiofilter/radio1938/radio_buffer_io.h"
#include "calibration_report.h"
#include "gmeaudio.h"
#include "kssaudio.h"
#include "m4adecoder.h"
#include "media_formats.h"
#include "miniaudio.h"
#include "runtime_helpers.h"
#include "vgmaudio.h"
#include "miniaudio_file_path.h"

std::filesystem::path defaultRadioOutputFor(
    const std::filesystem::path& input) {
  return input.parent_path() / (toUtf8String(input.stem()) + ".radio.wav");
}

static std::filesystem::path resolveRenderOutputPath(
    const std::filesystem::path& input, const std::string& outArg) {
  if (outArg.empty()) {
    return defaultRadioOutputFor(input);
  }

  std::filesystem::path outPath = pathFromUtf8String(outArg);
  bool directoryHint =
      !outArg.empty() && (outArg.back() == '/' || outArg.back() == '\\');
  if (directoryHint ||
      (std::filesystem::exists(outPath) &&
       std::filesystem::is_directory(outPath))) {
    return outPath / defaultRadioOutputFor(input).filename();
  }

  if (!outPath.has_extension()) {
    outPath += ".wav";
  }
  return outPath;
}

void renderToFile(const Options& o, const std::filesystem::path& inputPath,
                  const std::filesystem::path& outputPath,
                  const Radio1938& radio1938Template, bool useRadio1938,
                  Radio1938* renderedRadioOut, bool writeOutput) {
  const uint32_t sampleRate = 48000;
  const uint32_t channels = o.mono ? 1 : 2;
  constexpr uint32_t kRadioProcessChannels = 1u;
  auto rebuildRadioFromTemplate = [&](Radio1938* target) {
    if (!target) return;
    *target = radio1938Template;
    target->init(kRadioProcessChannels, static_cast<float>(sampleRate),
                 static_cast<float>(o.bwHz), static_cast<float>(o.noise));
  };
  const bool useM4a = isM4aExt(inputPath);
  const bool useGme = isGmeExt(inputPath);
  const bool useVgm = isVgmExt(inputPath);
  const bool useKss = isKssExt(inputPath);
  constexpr uint32_t chunkFrames = 1024;
  ma_decoder decoder{};
  M4aDecoder m4a{};
  GmeAudioDecoder gme{};
  VgmAudioDecoder vgm{};
  KssAudioDecoder kss{};
  if (useM4a) {
    std::string error;
    if (!m4a.init(inputPath, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else if (useKss) {
    std::string error;
    if (!kss.init(inputPath, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else if (useVgm) {
    std::string error;
    if (!vgm.init(inputPath, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else if (useGme) {
    std::string error;
    if (!gme.init(inputPath, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else {
    ma_decoder_config decConfig =
        ma_decoder_config_init(ma_format_f32, channels, sampleRate);
    if (maDecoderInitFilePath(inputPath, &decConfig, &decoder) != MA_SUCCESS) {
      die("Failed to open input for decoding.");
    }
  }

  ma_encoder encoder{};
  ma_encoder_config encConfig;
  std::vector<int16_t> out;
  if (writeOutput) {
    encConfig = ma_encoder_config_init(
        ma_encoding_format_wav, ma_format_s16, channels, sampleRate);
    if (maEncoderInitFilePath(outputPath, &encConfig, &encoder) != MA_SUCCESS) {
      if (useM4a) {
        m4a.uninit();
      } else if (useKss) {
        kss.uninit();
      } else if (useVgm) {
        vgm.uninit();
      } else if (useGme) {
        gme.uninit();
      } else {
        ma_decoder_uninit(&decoder);
      }
      die("Failed to open output for encoding.");
    }
    out.resize(static_cast<size_t>(chunkFrames * channels));
  }

  auto uninitEncoder = [&]() {
    if (!writeOutput) return;
    ma_encoder_uninit(&encoder);
  };

  auto uninitDecoder = [&]() {
    if (useM4a) {
      m4a.uninit();
    } else if (useKss) {
      kss.uninit();
    } else if (useVgm) {
      vgm.uninit();
    } else if (useGme) {
      gme.uninit();
    } else {
      ma_decoder_uninit(&decoder);
    }
  };

  const bool usePreviewRender = !o.dry && useRadio1938 && writeOutput;
  const bool useCalibrationRender =
      !o.dry && useRadio1938 && (renderedRadioOut || o.calibrationReport);
  RadioAmIngressConfig radioAmIngress;
  RadioPreviewConfig radioPreviewConfig;
  radioPreviewConfig.programBandwidthHz = 0.48f * static_cast<float>(o.bwHz);

  auto renderRadio = std::make_unique<Radio1938>();
  rebuildRadioFromTemplate(renderRadio.get());

  std::unique_ptr<Radio1938> calibrationRadio;
  if (useCalibrationRender) {
    calibrationRadio = std::make_unique<Radio1938>();
    rebuildRadioFromTemplate(calibrationRadio.get());
  }

  RadioPreviewPipeline radioPreview;
  if (usePreviewRender) {
    radioPreview.initialize(*renderRadio, radioAmIngress, radioPreviewConfig,
                            static_cast<float>(sampleRate));
  }
  if (calibrationRadio) {
    calibrationRadio->warmInputCarrier(radioAmIngress.receivedCarrierRmsVolts);
  }

  std::vector<float> buffer(chunkFrames * channels);
  std::vector<float> calibrationMono;

  while (true) {
    uint64_t framesRead = 0;
    if (useM4a) {
      if (!m4a.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        uninitEncoder();
        m4a.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else if (useKss) {
      if (!kss.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        uninitEncoder();
        kss.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else if (useVgm) {
      if (!vgm.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        uninitEncoder();
        vgm.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else if (useGme) {
      if (!gme.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        uninitEncoder();
        gme.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else {
      ma_uint64 framesReadMa = 0;
      ma_result res = ma_decoder_read_pcm_frames(&decoder, buffer.data(),
                                                 chunkFrames, &framesReadMa);
      framesRead = framesReadMa;
      if (framesRead == 0 || res == MA_AT_END) break;
    }

    if (calibrationRadio) {
      calibrationMono.resize(static_cast<size_t>(framesRead));
      for (uint32_t frame = 0; frame < static_cast<uint32_t>(framesRead);
           ++frame) {
        calibrationMono[frame] = sampleInterleavedToMono(
            buffer.data(), frame, static_cast<int>(channels));
      }
      calibrationRadio->processAmAudio(
          calibrationMono.data(), calibrationMono.data(),
          static_cast<uint32_t>(framesRead),
          radioAmIngress.receivedCarrierRmsVolts,
          radioAmIngress.modulationIndex);
    }

    if (usePreviewRender) {
      radioPreview.runBlock(*renderRadio, buffer.data(),
                            static_cast<uint32_t>(framesRead), channels);
    }

    if (writeOutput) {
      for (size_t i = 0; i < static_cast<size_t>(framesRead * channels); ++i) {
        float v = std::clamp(buffer[i], -1.0f, 1.0f);
        out[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
      }
      ma_uint64 framesWritten = 0;
      ma_encoder_write_pcm_frames(&encoder, out.data(),
                                  static_cast<ma_uint64>(framesRead),
                                  &framesWritten);
    }
  }

  uninitEncoder();
  uninitDecoder();

  if (o.calibrationReport && calibrationRadio &&
      calibrationRadio->calibration.enabled) {
    printCalibrationReport(*calibrationRadio, "Calibration report:");
  }

  if (renderedRadioOut) {
    if (calibrationRadio) {
      *renderedRadioOut = *calibrationRadio;
    } else {
      *renderedRadioOut = *renderRadio;
    }
  }
}

int runRenderRadioCli(const Options& o) {
  if (o.input.empty()) {
    die("render-radio requires an input file path.");
  }

  std::filesystem::path inputPath = pathFromUtf8String(o.input);
  requireSupportedAudioInputFile(inputPath);
  std::filesystem::path outputPath = resolveRenderOutputPath(inputPath, o.output);

  Radio1938 radio1938Template;
  radio1938Template.init(1, 48000.0f, static_cast<float>(o.bwHz),
                         static_cast<float>(o.noise));
  if (!o.radioSettingsPath.empty()) {
    std::string radioIniError;
    if (!applyRadioSettingsIni(radio1938Template, o.radioSettingsPath,
                               o.radioPresetName, &radioIniError)) {
      die("Failed to apply radio settings from '" + o.radioSettingsPath +
          "': " + radioIniError);
    }
  }
  if (o.calibrationReport || o.measureNodeSteps) {
    radio1938Template.setCalibrationEnabled(true);
  }

  Options renderOpt = o;
  renderOpt.input = toUtf8String(inputPath);
  renderOpt.output = toUtf8String(outputPath);

  logLine("Radioify");
  logLine("  Mode:   render-radio");
  logLine("  Input:  " + renderOpt.input);
  logLine("  Output: " + renderOpt.output);
  logLine(std::string("  Chain:  ") + (renderOpt.dry ? "dry" : "radio"));
  if (o.measureNodeSteps) {
    logLine("  Measure: Node-step sweep (enabled)");
  }
  logLine(std::string("  Report: ") +
          ((o.calibrationReport || o.measureNodeSteps) ? "calibration"
                                                       : "off"));
  if (o.measureNodeSteps) {
    logLine("Measuring node-step variants...");
    printNodeStepSummaryHeader();
    Radio1938 baselineRadio;
    renderOpt.calibrationReport = o.calibrationReport;
    renderToFile(renderOpt, inputPath, outputPath, radio1938Template, true,
                 &baselineRadio, false);
    printNodeStepSummaryLine("baseline", baselineRadio, nullptr);
    if (renderOpt.calibrationReport) {
      printCalibrationReport(baselineRadio, "Calibration report [baseline]:");
    }
    bool validationFailed = baselineRadio.calibration.validationFailed;
    for (size_t passIndex = 0;
         passIndex <= static_cast<size_t>(PassId::OutputClip); ++passIndex) {
      PassId passId = static_cast<PassId>(passIndex);
      if (!radio1938Template.graph.isEnabled(passId)) continue;
      std::string passName = std::string(Radio1938::passName(passId));
      if (passName == "Unknown") continue;
      Radio1938 stepTemplate = radio1938Template;
      stepTemplate.graph.setEnabled(passId, false);
      Radio1938 stepRadio;
      renderToFile(renderOpt, inputPath, outputPath, stepTemplate, true,
                   &stepRadio, false);
      printNodeStepSummaryLine(passName, stepRadio, &baselineRadio);
      if (renderOpt.calibrationReport) {
        printCalibrationReport(stepRadio,
                               std::string("Calibration report [disable ") +
                                   passName + "]:");
      }
      if (stepRadio.calibration.validationFailed) {
        validationFailed = true;
      }
    }
    logLine("Done.");
    if (validationFailed) {
      logLine("Validation failed.");
      return 2;
    }
    return 0;
  }

  renderOpt.calibrationReport = o.calibrationReport;
  Radio1938 renderedRadio;
  renderToFile(renderOpt, inputPath, outputPath, radio1938Template, true,
               &renderedRadio);
  logLine("Done.");
  if (renderOpt.calibrationReport && renderedRadio.calibration.enabled &&
      renderedRadio.calibration.validationFailed) {
    logLine("Validation failed.");
    return 2;
  }
  return 0;
}

static std::filesystem::path defaultMelodyOutputFor(
    const std::filesystem::path& input) {
  std::string base = toUtf8String(input.stem());
  return input.parent_path() / (base + ".melody");
}

static std::filesystem::path resolveExtractOutputPath(
    const std::filesystem::path& input, const std::string& outArg) {
  if (outArg.empty()) {
    return defaultMelodyOutputFor(input);
  }

  std::filesystem::path outPath = pathFromUtf8String(outArg);
  bool directoryHint =
      !outArg.empty() && (outArg.back() == '/' || outArg.back() == '\\');
  if (directoryHint ||
      (std::filesystem::exists(outPath) &&
       std::filesystem::is_directory(outPath))) {
    return outPath / (toUtf8String(input.stem()) + ".melody");
  }

  if (!outPath.has_extension()) {
    outPath += ".melody";
  }
  return outPath;
}

int runExtractSheetCli(const Options& o,
                       const AudioPlaybackConfig& audioConfig) {
  if (o.input.empty()) {
    die("extract-sheet requires an input file path.");
  }

  std::filesystem::path inputPath = pathFromUtf8String(o.input);
  requireSupportedAudioInputFile(inputPath);
  std::filesystem::path outputPath = resolveExtractOutputPath(inputPath, o.output);

  audioInit(audioConfig);

  std::string error;
  int lastPct = -1;
  auto progress = [&](float p) {
    int pct = static_cast<int>(std::lround(std::clamp(p, 0.0f, 1.0f) * 100.0f));
    if (pct == lastPct) return;
    lastPct = pct;
    std::cout << "\rAnalyzing: " << pct << "%" << std::flush;
  };

  bool ok = audioAnalyzeFileToMelodyFile(
      inputPath, std::max(0, o.trackIndex), outputPath, progress, &error);
  std::cout << "\n";
  audioShutdown();

  if (!ok) {
    die(error.empty() ? "Failed to extract melody sheet." : error);
  }

  logLine("Extract complete.");
  logLine("  Input:  " + toUtf8String(inputPath));
  std::filesystem::path midiOutputPath = outputPath;
  midiOutputPath.replace_extension(".mid");
  logLine("  Output: " + toUtf8String(outputPath) + " + " +
          toUtf8String(midiOutputPath));
  return 0;
}
