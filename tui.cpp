#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_common.h"
#include "asciiart.h"
#include "audioplayback.h"
#include "browsermeta.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "vgmaudio.h"
#include "kssaudio.h"
#include "m4adecoder.h"
#include "miniaudio.h"
#include "optionsbrowser.h"
#include "psfaudio.h"
#include "radio.h"
#include "radiopreview.h"
#include "tracklist.h"
#include "loopsplit_cli.h"
#include "loopsplit_ui.h"
#include "ui_helpers.h"
#include "videoplayback.h"
#include "videowindow.h"

#include "tui.h"
#include "timing_log.h"

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

struct TrackBrowserState {
  bool active = false;
  std::filesystem::path file;
  std::vector<TrackEntry> tracks;
};

static TrackBrowserState gTrackBrowser;

static std::filesystem::path normalizeTrackBrowserPath(
    std::filesystem::path path) {
  if (!path.has_parent_path()) {
    path = std::filesystem::path(".") / path;
  }
  return path;
}

static bool isTrackBrowserActive(const BrowserState& state) {
  return gTrackBrowser.active && state.dir == gTrackBrowser.file;
}

static const TrackEntry* findTrackEntry(int trackIndex) {
  if (trackIndex < 0) return nullptr;
  if (trackIndex < static_cast<int>(gTrackBrowser.tracks.size())) {
    const auto& entry = gTrackBrowser.tracks[static_cast<size_t>(trackIndex)];
    if (entry.index == trackIndex) {
      return &entry;
    }
  }
  for (const auto& entry : gTrackBrowser.tracks) {
    if (entry.index == trackIndex) return &entry;
  }
  return nullptr;
}

static bool isSupportedImageExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp";
}

static bool isVideoExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".mp4" || ext == ".webm" || ext == ".mov" || ext == ".mkv";
}

static bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4" || ext == ".mov" || ext == ".kss" ||
         ext == ".nsf" ||
#if !RADIOIFY_DISABLE_GSF_GPL
         ext == ".gsf" || ext == ".minigsf" ||
#endif
         ext == ".mid" || ext == ".midi" ||
         ext == ".vgm" || ext == ".vgz" || ext == ".psf" ||
         ext == ".minipsf" ||
         ext == ".psf2" || ext == ".minipsf2";
}

static bool isMiniaudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

static bool isGmeExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".nsf";
}

static bool isGsfExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
#if !RADIOIFY_DISABLE_GSF_GPL
  return ext == ".gsf" || ext == ".minigsf";
#else
  (void)ext;
  return false;
#endif
}

static bool isVgmExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".vgm" || ext == ".vgz";
}

static bool isKssExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".kss";
}

static bool isPsfExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".psf" || ext == ".minipsf" || ext == ".psf2" ||
         ext == ".minipsf2";
}

static bool isSupportedMediaExt(const std::filesystem::path& p) {
  return isSupportedAudioExt(p) || isSupportedImageExt(p) || isVideoExt(p);
}

static bool isM4aExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm" || ext == ".mov";
}

static bool listTracksForFile(const std::filesystem::path& path,
                              std::vector<TrackEntry>* tracks,
                              std::string* error) {
  if (isKssExt(path)) {
    return kssListTracks(path, tracks, error);
  }
  if (isGmeExt(path)) {
    return gmeListTracks(path, tracks, error);
  }
  if (isGsfExt(path)) {
    return gsfListTracks(path, tracks, error);
  }
  if (isVgmExt(path)) {
    return vgmListTracks(path, tracks, error);
  }
  if (isPsfExt(path)) {
    return psfListTracks(path, tracks, error);
  }
  return false;
}

static void validateInputFile(const std::filesystem::path& p) {
  if (p.empty()) die("Missing input file path.");
  if (!std::filesystem::exists(p)) die("Input file not found: " + p.string());
  if (std::filesystem::is_directory(p))
    die("Input path must be a file: " + p.string());
  if (!isSupportedAudioExt(p)) {
    die("Unsupported input format '" + p.extension().string() +
        "'. Supported: .wav, .mp3, .flac, .m4a, .webm, .mp4, .mov, .kss, .nsf, .mid, .midi, "
#if !RADIOIFY_DISABLE_GSF_GPL
        ".gsf, .minigsf, "
#endif
        ".vgm, .vgz, .psf, .minipsf, .psf2, .minipsf2.");
  }
}

static std::filesystem::path defaultRadioOutputFor(
    const std::filesystem::path& input) {
  return input.parent_path() / (input.stem().string() + ".radio.wav");
}

static std::filesystem::path resolveRenderOutputPath(
    const std::filesystem::path& input, const std::string& outArg) {
  if (outArg.empty()) {
    return defaultRadioOutputFor(input);
  }

  std::filesystem::path outPath(outArg);
  bool directoryHint =
      !outArg.empty() &&
      (outArg.back() == '/' || outArg.back() == '\\');
  if (directoryHint ||
      (std::filesystem::exists(outPath) && std::filesystem::is_directory(outPath))) {
    return outPath / defaultRadioOutputFor(input).filename();
  }

  if (!outPath.has_extension()) {
    outPath += ".wav";
  }
  return outPath;
}

static std::vector<FileEntry> listEntries(const std::filesystem::path& dir) {
  std::vector<FileEntry> entries;
  std::vector<FileEntry> items;

#ifdef _WIN32
  if (dir.empty() || dir == dir.root_path()) {
    for (const auto& drive : listDriveEntries()) {
      entries.push_back(FileEntry{drive.label, drive.path, true});
    }
  }
#endif

  if (dir.has_parent_path() && dir != dir.root_path()) {
    entries.push_back(FileEntry{"..", dir.parent_path(), true});
  }

  try {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const auto& p = entry.path();
      if (entry.is_directory()) {
        items.push_back(FileEntry{toUtf8String(p.filename()), p, true});
      } else if (entry.is_regular_file() && isSupportedMediaExt(p)) {
        items.push_back(FileEntry{toUtf8String(p.filename()), p, false});
      }
    }
  } catch (...) {
    return entries;
  }

  entries.insert(entries.end(), items.begin(), items.end());
  return entries;
}

static void refreshBrowser(BrowserState& state,
                           const std::string& initialName) {
  bool optionsActive = optionsBrowserRefresh(state);
  if (!optionsActive && isTrackBrowserActive(state)) {
    state.entries.clear();
    if (state.dir.has_parent_path()) {
      state.entries.push_back(FileEntry{"..", state.dir.parent_path(), true});
    }
    int digits = trackLabelDigits(gTrackBrowser.tracks.size());
    for (const auto& track : gTrackBrowser.tracks) {
      FileEntry entry;
      entry.name = formatTrackLabel(track, digits);
      entry.path = state.dir;
      entry.isDir = false;
      entry.trackIndex = track.index;
      state.entries.push_back(std::move(entry));
    }
  } else if (!optionsActive) {
    if (gTrackBrowser.active && state.dir != gTrackBrowser.file) {
      gTrackBrowser.active = false;
    }
    state.entries = listEntries(state.dir);
  }

  if (!state.filter.empty()) {
    std::string lowFilter = toLower(state.filter);
    state.entries.erase(
        std::remove_if(state.entries.begin(), state.entries.end(),
                       [&](const FileEntry& e) {
                         if (e.name == "..") return false;
                         return toLower(e.name).find(lowFilter) ==
                                std::string::npos;
                       }),
        state.entries.end());
  }

  if (!state.entries.empty() && !optionsActive) {
    auto start = state.entries.begin();
    if (start->name == "..") ++start;

    std::sort(start, state.entries.end(), [&](const FileEntry& a, const FileEntry& b) {
      if (a.isDir != b.isDir) return a.isDir > b.isDir;
      bool aLessB = false;
      if (state.sortMode == BrowserState::SortMode::Date) {
        try {
          aLessB = std::filesystem::last_write_time(a.path) <
                   std::filesystem::last_write_time(b.path);
        } catch (...) {
          aLessB = toLower(a.name) < toLower(b.name);
        }
      } else if (state.sortMode == BrowserState::SortMode::Size) {
        try {
          if (!a.isDir && !b.isDir) {
            aLessB = std::filesystem::file_size(a.path) <
                     std::filesystem::file_size(b.path);
          } else {
            aLessB = toLower(a.name) < toLower(b.name);
          }
        } catch (...) {
          aLessB = toLower(a.name) < toLower(b.name);
        }
      } else {
        aLessB = toLower(a.name) < toLower(b.name);
      }

      if (state.sortDescending) return !aLessB;
      return aLessB;
    });
  }

  if (state.entries.empty()) {
    state.selected = 0;
    state.scrollRow = 0;
    return;
  }

  if (state.selected < 0 ||
      state.selected >= static_cast<int>(state.entries.size())) {
    state.selected = 0;
  }
  state.scrollRow = 0;

  if (!initialName.empty()) {
    for (size_t i = 0; i < state.entries.size(); ++i) {
      if (toLower(state.entries[i].name) == toLower(initialName)) {
        state.selected = static_cast<int>(i);
        break;
      }
    }
  }
}

static std::string buildTrackSelectionMeta(const BrowserState& browser) {
  if (browser.entries.empty()) return "";
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  std::string name = entry.name;
  if (entry.isDir && name != "..") name += "/";

  std::string sortLabel = "Name";
  if (browser.sortMode == BrowserState::SortMode::Date) sortLabel = "Date";
  else if (browser.sortMode == BrowserState::SortMode::Size) sortLabel = "Size";

  std::string dirArrow = browser.sortDescending ? " \xE2\x86\x93" : " \xE2\x86\x91";
  std::string metaLine = " [" + sortLabel + dirArrow + "]";
  if (browser.filterActive || !browser.filter.empty()) {
    metaLine += " [Filter: " + browser.filter + (browser.filterActive ? "_" : "") + "]";
  }

  metaLine += " Selected: " + name;
  if (entry.trackIndex >= 0) {
    const TrackEntry* track = findTrackEntry(entry.trackIndex);
    if (track && track->lengthMs > 0) {
      metaLine += "  " + formatTime(static_cast<double>(track->lengthMs) / 1000.0);
    }
    if (!gTrackBrowser.tracks.empty()) {
      metaLine += "  Track " + std::to_string(entry.trackIndex + 1) + "/" +
                  std::to_string(gTrackBrowser.tracks.size());
    }
  }
  return metaLine;
}

static bool showAsciiArt(const std::filesystem::path& file, ConsoleInput& input,
                         ConsoleScreen& screen, const Style& baseStyle,
                         const Style& accentStyle, const Style& dimStyle) {
  AsciiArt art;
  std::string error;
  bool ok = false;

  auto renderFrame = [&]() {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    const int headerLines = 2;
    const int footerLines = 1;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    error.clear();
    ok = renderAsciiArt(file, width, maxHeight, art, &error);

    screen.clear(baseStyle);
    std::string title = "Preview: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    screen.writeText(0, 1, fitLine("Press any key to return", width), dimStyle);

    if (!ok) {
      screen.writeText(0, artTop, fitLine("Failed to open image.", width),
                       dimStyle);
      if (!error.empty() && artTop + 1 < height) {
        screen.writeText(0, artTop + 1, fitLine(error, width), dimStyle);
      }
      screen.draw();
      return;
    }

    int artWidth = std::min(art.width, width);
    int artHeight = std::min(art.height, maxHeight);
    int artX = std::max(0, (width - artWidth) / 2);

    for (int y = 0; y < artHeight; ++y) {
      for (int x = 0; x < artWidth; ++x) {
        const auto& cell = art.cells[static_cast<size_t>(y * art.width + x)];
        Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
        screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
      }
    }
    screen.draw();
  };

  renderFrame();

  InputEvent ev{};
  while (true) {
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
        renderFrame();
        continue;
      }
      if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                               FROM_LEFT_2ND_BUTTON_PRESSED |
                               FROM_LEFT_3RD_BUTTON_PRESSED |
                               FROM_LEFT_4TH_BUTTON_PRESSED;
        bool backPressed = (mouse.buttonState & backMask) != 0;
        if (backPressed) {
          return ok;
        }
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        return ok;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

static void renderToFile(const Options& o, const Radio1938& radio1938Template,
                         bool useRadio1938,
                         Radio1938* renderedRadioOut = nullptr) {
  const uint32_t sampleRate = 48000;
  const uint32_t channels = o.mono ? 1 : 2;
  constexpr uint32_t kRadioProcessChannels = 1u;
  auto rebuildRadioFromTemplate = [&](Radio1938* target) {
    if (!target) return;
    target->preset = radio1938Template.preset;
    target->identity = radio1938Template.identity;
    target->setCalibrationEnabled(radio1938Template.calibration.enabled);
    target->init(kRadioProcessChannels, static_cast<float>(sampleRate),
                 static_cast<float>(o.bwHz), static_cast<float>(o.noise));
  };
  const bool useM4a = isM4aExt(o.input);
  const bool useGme = isGmeExt(o.input);
  const bool useVgm = isVgmExt(o.input);
  const bool useKss = isKssExt(o.input);
  ma_decoder decoder{};
  M4aDecoder m4a{};
  GmeAudioDecoder gme{};
  VgmAudioDecoder vgm{};
  KssAudioDecoder kss{};
  if (useM4a) {
    std::string error;
    if (!m4a.init(o.input, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else if (useKss) {
    std::string error;
    if (!kss.init(o.input, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else if (useVgm) {
    std::string error;
    if (!vgm.init(o.input, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else if (useGme) {
    std::string error;
    if (!gme.init(o.input, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else {
    ma_decoder_config decConfig =
        ma_decoder_config_init(ma_format_f32, channels, sampleRate);
    if (ma_decoder_init_file(o.input.c_str(), &decConfig, &decoder) !=
        MA_SUCCESS) {
      die("Failed to open input for decoding.");
    }
  }

  ma_encoder encoder{};
  ma_encoder_config encConfig = ma_encoder_config_init(
      ma_encoding_format_wav, ma_format_s16, channels, sampleRate);
  if (ma_encoder_init_file(o.output.c_str(), &encConfig, &encoder) !=
      MA_SUCCESS) {
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

  auto radio1938 = std::make_unique<Radio1938>();
  rebuildRadioFromTemplate(radio1938.get());
  PcmToIfPreviewModulator radioPreview;
  radioPreview.init(*radio1938, static_cast<float>(sampleRate),
                    static_cast<float>(o.bwHz));
  constexpr uint32_t chunkFrames = 1024;
  std::vector<float> buffer(chunkFrames * channels);
  std::vector<int16_t> out(buffer.size());

  while (true) {
    uint64_t framesRead = 0;
    if (useM4a) {
      if (!m4a.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        ma_encoder_uninit(&encoder);
        m4a.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else if (useKss) {
      if (!kss.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        ma_encoder_uninit(&encoder);
        kss.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else if (useVgm) {
      if (!vgm.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        ma_encoder_uninit(&encoder);
        vgm.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else if (useGme) {
      if (!gme.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        ma_encoder_uninit(&encoder);
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

    if (!o.dry && useRadio1938) {
      radioPreview.processBlock(*radio1938, buffer.data(),
                                static_cast<uint32_t>(framesRead), channels);
    }

    for (size_t i = 0; i < static_cast<size_t>(framesRead * channels); ++i) {
      float v = std::clamp(buffer[i], -1.0f, 1.0f);
      out[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
    }

    ma_uint64 framesWritten = 0;
    ma_encoder_write_pcm_frames(&encoder, out.data(),
                                static_cast<ma_uint64>(framesRead),
                                &framesWritten);
  }

  ma_encoder_uninit(&encoder);
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

  if (o.calibrationReport && useRadio1938 && radio1938->calibration.enabled) {
    auto oldFlags = std::cout.flags();
    auto oldPrecision = std::cout.precision();
    std::cout << std::fixed << std::setprecision(3);
    logLine("Calibration report:");
    for (size_t i = 0; i < radio1938->calibration.stages.size(); ++i) {
      StageId id = static_cast<StageId>(i);
      const auto& stage = radio1938->calibration.stages[i];
      if (stage.sampleCount == 0) continue;
      std::cout << "  " << Radio1938::stageName(id)
                << " rms_in=" << stage.rmsIn
                << " rms_out=" << stage.rmsOut
                << " mean_out=" << stage.meanOut
                << " peak_out=" << stage.peakOut
                << " crest_out=" << stage.crestOut
                << " centroid_hz=" << stage.spectralCentroidHz
                << " bw3_hz=" << stage.bandwidth3dBHz
                << " bw6_hz=" << stage.bandwidth6dBHz
                << " clips_in=" << stage.clipCountIn
                << " clips_out=" << stage.clipCountOut
                << "\n";
    }
    std::cout << "  limiter duty=" << radio1938->calibration.limiterDutyCycle
              << " avg_gr=" << radio1938->calibration.limiterAverageGainReduction
              << " max_gr=" << radio1938->calibration.limiterMaxGainReduction
              << " avg_gr_db="
              << radio1938->calibration.limiterAverageGainReductionDb
              << " max_gr_db="
              << radio1938->calibration.limiterMaxGainReductionDb
              << "\n";
    const auto& receiverStage =
        radio1938->calibration.stages[static_cast<size_t>(StageId::ReceiverCircuit)];
    std::cout << "  receiver_out_rms=" << receiverStage.rmsOut
              << " receiver_out_peak=" << receiverStage.peakOut
              << " interstage_secondary_rms="
              << radio1938->calibration.interstageSecondaryRmsVolts
              << " interstage_secondary_peak="
              << radio1938->calibration.interstageSecondaryPeakVolts
              << "\n";
    std::cout << "  driver_grid_positive="
              << radio1938->calibration.driverGridPositiveFraction
              << " output_grid_a_positive="
              << radio1938->calibration.outputGridAPositiveFraction
              << " output_grid_b_positive="
              << radio1938->calibration.outputGridBPositiveFraction
              << " output_grid_positive="
              << radio1938->calibration.outputGridPositiveFraction
              << " max_mixer_ip=" << radio1938->calibration.maxMixerPlateCurrentAmps
              << " max_receiver_ip="
              << radio1938->calibration.maxReceiverPlateCurrentAmps
              << " max_driver_ip="
              << radio1938->calibration.maxDriverPlateCurrentAmps
              << " max_output_ip_a="
              << radio1938->calibration.maxOutputPlateCurrentAAmps
              << " max_output_ip_b="
              << radio1938->calibration.maxOutputPlateCurrentBAmps
              << " max_speaker_v="
              << radio1938->calibration.maxSpeakerSecondaryVolts
              << " max_digital_output="
              << radio1938->calibration.maxDigitalOutput
              << "\n";
    std::cout << "  validation failed="
              << (radio1938->calibration.validationFailed ? 1 : 0)
              << " driver_grid_over="
              << (radio1938->calibration.validationDriverGridPositive ? 1 : 0)
              << " output_grid_a_over="
              << (radio1938->calibration.validationOutputGridAPositive ? 1 : 0)
              << " output_grid_b_over="
              << (radio1938->calibration.validationOutputGridBPositive ? 1 : 0)
              << " output_grid_over="
              << (radio1938->calibration.validationOutputGridPositive ? 1 : 0)
              << " interstage_over="
              << (radio1938->calibration.validationInterstageSecondary ? 1 : 0)
              << " speaker_over="
              << (radio1938->calibration.validationSpeakerOverReference ? 1 : 0)
              << " dc_shift="
              << (radio1938->calibration.validationDcShift ? 1 : 0)
              << " digital_clip="
              << (radio1938->calibration.validationDigitalClip ? 1 : 0)
              << "\n";
    std::cout.flags(oldFlags);
    std::cout.precision(oldPrecision);
  }

  if (renderedRadioOut) {
    *renderedRadioOut = *radio1938;
  }
}

static int runRenderRadioCli(const Options& o) {
  if (o.input.empty()) {
    die("render-radio requires an input file path.");
  }

  std::filesystem::path inputPath(o.input);
  validateInputFile(inputPath);
  std::filesystem::path outputPath = resolveRenderOutputPath(inputPath, o.output);

  Radio1938 radio1938Template;
  radio1938Template.init(1, 48000.0f, static_cast<float>(o.bwHz),
                         static_cast<float>(o.noise));
  if (o.calibrationReport) {
    radio1938Template.setCalibrationEnabled(true);
  }

  Options renderOpt = o;
  renderOpt.input = inputPath.string();
  renderOpt.output = outputPath.string();

  logLine("Radioify");
  logLine("  Mode:   render-radio");
  logLine("  Input:  " + renderOpt.input);
  logLine("  Output: " + renderOpt.output);
  logLine(std::string("  Chain:  ") + (renderOpt.dry ? "dry" : "radio"));
  logLine(std::string("  Report: ") +
          (renderOpt.calibrationReport ? "calibration" : "off"));
  logLine("Rendering output...");
  Radio1938 renderedRadio;
  renderToFile(renderOpt, radio1938Template, true, &renderedRadio);
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
  std::string base = input.stem().string();
  return input.parent_path() / (base + ".melody");
}

static std::filesystem::path resolveExtractOutputPath(
    const std::filesystem::path& input, const std::string& outArg) {
  if (outArg.empty()) {
    return defaultMelodyOutputFor(input);
  }

  std::filesystem::path outPath(outArg);
  bool directoryHint =
      !outArg.empty() &&
      (outArg.back() == '/' || outArg.back() == '\\');
  if (directoryHint ||
      (std::filesystem::exists(outPath) && std::filesystem::is_directory(outPath))) {
    return outPath / (input.stem().string() + ".melody");
  }

  if (!outPath.has_extension()) {
    outPath += ".melody";
  }
  return outPath;
}

static int runExtractSheetCli(const Options& o,
                              const AudioPlaybackConfig& audioConfig) {
  if (o.input.empty()) {
    die("extract-sheet requires an input file path.");
  }

  std::filesystem::path inputPath(o.input);
  validateInputFile(inputPath);
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

  bool ok =
      audioAnalyzeFileToMelodyFile(inputPath, std::max(0, o.trackIndex), outputPath,
                                  progress, &error);
  std::cout << "\n";
  audioShutdown();

  if (!ok) {
    die(error.empty() ? "Failed to extract melody sheet." : error);
  }

  logLine("Extract complete.");
  logLine("  Input:  " + inputPath.string());
  std::filesystem::path midiOutputPath = outputPath;
  midiOutputPath.replace_extension(".mid");
  logLine("  Output: " + outputPath.string() + " + " + midiOutputPath.string());
  return 0;
}

int runTui(Options o) {
  configureFfmpegVideoLog({});

  AudioPlaybackConfig audioConfig;
  audioConfig.enableAudio = o.enableAudio;
  audioConfig.enableRadio = o.enableRadio;
  audioConfig.mono = o.mono;
  audioConfig.dry = o.dry;
  audioConfig.bwHz = o.bwHz;
  audioConfig.noise = o.noise;

  if (o.extractSheet) {
    return runExtractSheetCli(o, audioConfig);
  }
  if (o.splitLoop) {
    return runSplitLoopCli(o);
  }
  if (o.renderRadio) {
    return runRenderRadioCli(o);
  }

  float lpHz = static_cast<float>(o.bwHz);
  const uint32_t sampleRate = 48000;
  const uint32_t baseChannels = o.mono ? 1 : 2;
  uint32_t channels = baseChannels;

  auto radio1938Template = std::make_unique<Radio1938>();
  radio1938Template->init(1, static_cast<float>(sampleRate), lpHz,
                          static_cast<float>(o.noise));

  std::filesystem::path startDir = std::filesystem::current_path();
  std::string initialName;
  if (!o.input.empty()) {
    std::filesystem::path inputPath(o.input);
    if (std::filesystem::exists(inputPath)) {
      if (std::filesystem::is_directory(inputPath)) {
        startDir = inputPath;
        o.input.clear();
      } else {
        startDir = inputPath.parent_path();
        initialName = toUtf8String(inputPath.filename());
      }
    }
  }

  BrowserState browser;
  browser.dir = startDir;
  refreshBrowser(browser, initialName);

  ConsoleInput input;
  input.init();

  ConsoleScreen screen;
  screen.init();

  VideoWindow tuiWindow;
  bool windowTuiEnabled = o.enableWindow;
  if (windowTuiEnabled) {
    if (!tuiWindow.Open(1280, 720, "Radioify TUI", false)) {
      windowTuiEnabled = false;
    } else {
      tuiWindow.SetCaptureAllMouseInput(true);
      tuiWindow.SetVsync(true);
      tuiWindow.ShowWindow(true);
    }
  }

  audioInit(audioConfig);

  VideoPlaybackConfig videoConfig;
  videoConfig.enableAscii = o.enableAscii;
  videoConfig.enableAudio = o.enableAudio;
  videoConfig.debugOverlay = true;
  // If dedicated window-TUI is active, keep video playback out of the legacy
  // window path. If window-TUI could not be opened, fall back to normal
  // window playback behavior.
  videoConfig.enableWindow = o.enableWindow && !windowTuiEnabled;

  std::filesystem::path pendingImage;
  bool hasPendingImage = false;
  std::filesystem::path pendingVideo;
  bool hasPendingVideo = false;

  if (!o.input.empty() && o.play && std::filesystem::exists(o.input)) {
    std::filesystem::path inputPath(o.input);
    if (!std::filesystem::is_directory(inputPath)) {
      if (isSupportedImageExt(inputPath)) {
        pendingImage = inputPath;
        hasPendingImage = true;
      } else if (isVideoExt(inputPath)) {
        pendingVideo = inputPath;
        hasPendingVideo = true;
      } else {
        if (isKssExt(inputPath) || isGmeExt(inputPath) || isVgmExt(inputPath) ||
            isPsfExt(inputPath)) {
          std::filesystem::path trackPath =
              normalizeTrackBrowserPath(inputPath);
          std::vector<TrackEntry> tracks;
          std::string error;
          bool listed = listTracksForFile(trackPath, &tracks, &error);
          if (listed) {
            gTrackBrowser.file = trackPath;
            gTrackBrowser.tracks = tracks;
            gTrackBrowser.active = tracks.size() > 1;
          } else {
            gTrackBrowser.active = false;
            gTrackBrowser.tracks.clear();
          }
          if (listed && tracks.size() > 1) {
            browser.dir = trackPath;
            browser.selected = 0;
            browser.scrollRow = 0;
            browser.filter.clear();
            browser.filterActive = false;
            refreshBrowser(browser, "");
          } else {
            audioStartFile(inputPath);
          }
        } else {
          audioStartFile(inputPath);
        }
      }
    }
  }

  auto renderFile = [&](const std::filesystem::path& file) -> void {
    Options renderOpt = o;
    renderOpt.input = file.string();
    if (renderOpt.output.empty()) {
      renderOpt.output = defaultRadioOutputFor(file).string();
    }
    input.restore();
    screen.restore();
    logLine("Radioify");
    logLine(std::string("  Mode:   render"));
    logLine(std::string("  Input:  ") + renderOpt.input);
    logLine(std::string("  Output: ") + renderOpt.output);
    logLine("Rendering output...");
    renderToFile(renderOpt, *radio1938Template, audioIsRadioEnabled());
    logLine("Done.");
  };

  const Color kBgBase{12, 15, 20};
  const Style kStyleNormal{{215, 220, 226}, kBgBase};
  const Style kStyleHeader{{230, 238, 248}, {18, 28, 44}};
  const Style kStyleHeaderGlow{{255, 213, 118}, {22, 34, 52}};
  const Style kStyleHeaderHot{{255, 249, 214}, {38, 50, 72}};
  const Style kStyleAccent{{255, 214, 120}, kBgBase};
  const Style kStyleDim{{138, 144, 153}, kBgBase};
  const Style kStyleAlert{{255, 92, 92}, kBgBase};
  const Style kStyleDir{{110, 231, 183}, kBgBase};
  const Style kStyleHighlight{{15, 20, 28}, {230, 238, 248}};
  const Style kStyleBreadcrumbHover{{15, 20, 28}, {255, 214, 120}};
  const Style kStyleActionActive{{15, 20, 28}, {255, 214, 120}};
  const Color kProgressStart{110, 231, 183};
  const Color kProgressEnd{255, 214, 110};
  const Style kStyleProgressEmpty{{32, 38, 46}, {32, 38, 46}};
  const Style kStyleProgressFrame{{160, 170, 182}, kBgBase};

  screen.clear(kStyleNormal);
  screen.draw();

  bool running = true;
  bool dirty = true;
  auto lastDraw = std::chrono::steady_clock::now();
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  ActionStripLayout actionStrip;
  int actionHover = -1;
  int breadcrumbHover = -1;
  int headerLines = 0;
  int listTop = 0;
  int breadcrumbY = 0;
  const int footerLines = 5;
  int width = 0;
  int height = 0;
  int listHeight = 0;
  GridLayout layout;
  BreadcrumbLine breadcrumbLine;
  bool rendered = false;
  bool melodyVisualizationEnabled = false;
  std::deque<int> melodyHistoryMidi;
  std::deque<float> melodyHistoryConfidence;
  constexpr size_t kMelodyHistoryMaxSamples = 4096;
  std::filesystem::path lastMelodyTrack;
  int lastMelodyTrackIndex = -1;
  bool lastMelodyAnalysisRunning = false;
  double seekDisplaySec = -1.0;
  bool seekHoldActive = false;
  auto seekHoldStart = std::chrono::steady_clock::now();
  std::vector<ScreenCell> windowCells;
  auto markSeekHold = [&](double targetSec) {
    if (!std::isfinite(targetSec) || targetSec < 0.0) return;
    seekDisplaySec = targetSec;
    seekHoldActive = true;
    seekHoldStart = std::chrono::steady_clock::now();
  };
  auto midiToNoteName = [](int midi) {
    if (midi < 0 || midi > 127) return std::string("??");
    static const char* kNoteNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                      "F#", "G",  "G#", "A",  "A#", "B"};
    int note = midi % 12;
    if (note < 0) note += 12;
    int octave = midi / 12 - 1;
    return std::string(kNoteNames[static_cast<size_t>(note)]) +
           std::to_string(octave);
  };
  auto clearMelodyHistory = [&]() {
    melodyHistoryMidi.clear();
    melodyHistoryConfidence.clear();
  };
  auto pushMelodyHistory = [&](const AudioMelodyInfo& info) {
    int midi = (info.midiNote >= 0) ? info.midiNote : -1;
    melodyHistoryMidi.push_back(midi);
    melodyHistoryConfidence.push_back(std::clamp(info.confidence, 0.0f, 1.0f));
    while (melodyHistoryMidi.size() > kMelodyHistoryMaxSamples) {
      melodyHistoryMidi.pop_front();
      melodyHistoryConfidence.pop_front();
    }
  };

  if (hasPendingImage) {
    showAsciiArt(pendingImage, input, screen, kStyleNormal, kStyleAccent,
                 kStyleDim);
    dirty = true;
  }
  if (hasPendingVideo) {
    bool quitAppRequested = false;
    bool handled =
        showAsciiVideo(pendingVideo, input, screen, kStyleNormal, kStyleAccent,
                       kStyleDim, kStyleProgressEmpty, kStyleProgressFrame,
                       kProgressStart, kProgressEnd, videoConfig,
                       &quitAppRequested);
    if (quitAppRequested) {
      running = false;
    } else if (!handled) {
      audioStartFile(pendingVideo);
    }
    dirty = true;
  }

  struct CommandEntry {
    std::string label;
    std::string hotkey;
    bool enabled = true;
    std::function<void()> run;
  };

  struct FileContextMenuState {
    bool active = false;
    FileEntry entry{};
    int selected = 0;
    int anchorX = -1;
    int anchorY = -1;
  };

  struct FileContextMenuLayout {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int listY = 0;
    int rows = 0;
    bool valid = false;
  };

  struct MelodyExportTaskState {
    std::mutex mutex;
    std::thread worker;
    bool running = false;
    bool hasResult = false;
    bool success = false;
    float progress = 0.0f;
    std::string status;
    std::filesystem::path sourceFile;
    std::filesystem::path outputFile;
  };

  struct PaletteLayout {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int innerWidth = 0;
    int inputY = 0;
    int listY = 0;
    int listRows = 0;
    bool valid = false;
  };

  auto rebuildLayout = [&]() {
    screen.updateSize();
    width = std::max(40, screen.width());
    height = std::max(10, screen.height());
    bool browserInteractionEnabled = !melodyVisualizationEnabled;
    bool showHeaderLabel = browserInteractionEnabled &&
                           (optionsBrowserIsActive(browser) ||
                            isTrackBrowserActive(browser));
    headerLines = showHeaderLabel ? 2 : 1;
    if (browserInteractionEnabled) {
      listTop = headerLines + 1;
      breadcrumbY = listTop - 1;
    } else {
      listTop = headerLines;
      breadcrumbY = -1;
    }
    listHeight = height - listTop - footerLines;
    if (listHeight < 1) listHeight = 1;
    layout = buildLayout(browser, width, listHeight);
    if (layout.totalRows <= layout.rowsVisible) {
      browser.scrollRow = 0;
    } else {
      int maxScroll = layout.totalRows - layout.rowsVisible;
      browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
    }
    breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
    if (!browserInteractionEnabled) {
      breadcrumbHover = -1;
    } else if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
      breadcrumbHover = -1;
    }
  };

  FileContextMenuState fileContextMenu;
  FileContextMenuLayout fileContextLayout;
  MelodyExportTaskState melodyExportTask;
  LoopSplitTaskState loopSplitTask;

  auto isBackgroundTaskRunning = [&]() {
    bool running = false;
    {
      std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
      running = running || melodyExportTask.running;
    }
    {
      std::lock_guard<std::mutex> lock(loopSplitTask.mutex);
      running = running || loopSplitTask.running;
    }
    return running;
  };

  InputCallbacks callbacks;
  callbacks.onRefreshBrowser = [&](BrowserState& nextBrowser,
                                   const std::string& initialName) {
    refreshBrowser(nextBrowser, initialName);
  };
  callbacks.onPlayFile = [&](const std::filesystem::path& file) {
    OptionsBrowserResult optionsResult =
        optionsBrowserActivateSelection(browser);
    if (optionsResult == OptionsBrowserResult::Changed) {
      refreshBrowser(browser, "");
      return true;
    }
    if (optionsResult == OptionsBrowserResult::Handled) {
      return true;
    }
    if (isTrackBrowserActive(browser)) {
      if (!browser.entries.empty()) {
        int idx = std::clamp(browser.selected, 0,
                             static_cast<int>(browser.entries.size()) - 1);
        const auto& entry = browser.entries[static_cast<size_t>(idx)];
        if (entry.trackIndex >= 0) {
          return audioStartFile(file, entry.trackIndex);
        }
      }
      return true;
    }
    if (isSupportedImageExt(file)) {
      showAsciiArt(file, input, screen, kStyleNormal, kStyleAccent, kStyleDim);
      return true;
    }
    if (isVideoExt(file)) {
      bool quitAppRequested = false;
      bool handled = showAsciiVideo(
          file, input, screen, kStyleNormal, kStyleAccent, kStyleDim,
          kStyleProgressEmpty, kStyleProgressFrame, kProgressStart,
          kProgressEnd, videoConfig, &quitAppRequested);
      if (quitAppRequested) {
        running = false;
        return true;
      }
      if (handled) return true;
    }
    if (isKssExt(file) || isGmeExt(file) || isVgmExt(file) || isPsfExt(file)) {
      std::filesystem::path trackPath = normalizeTrackBrowserPath(file);
      std::vector<TrackEntry> tracks;
      std::string error;
      bool listed = listTracksForFile(trackPath, &tracks, &error);
      if (listed) {
        gTrackBrowser.file = trackPath;
        gTrackBrowser.tracks = tracks;
        gTrackBrowser.active = tracks.size() > 1;
      } else {
        gTrackBrowser.active = false;
        gTrackBrowser.tracks.clear();
      }
      if (listed && tracks.size() > 1) {
        browser.dir = trackPath;
        browser.selected = 0;
        browser.scrollRow = 0;
        browser.filter.clear();
        browser.filterActive = false;
        refreshBrowser(browser, "");
        return true;
      }
    }
    return audioStartFile(file);
  };
  callbacks.onOpenFileContextMenu = [&](const FileEntry& entry, int x, int y) {
    if (!o.play || entry.isDir || !isSupportedAudioExt(entry.path)) {
      return;
    }
    fileContextMenu.active = true;
    fileContextMenu.entry = entry;
    fileContextMenu.selected = 0;
    fileContextMenu.anchorX = x;
    fileContextMenu.anchorY = y;
    dirty = true;
  };
  callbacks.onRenderFile = [&](const std::filesystem::path& file) {
    renderFile(file);
    rendered = true;
  };
  callbacks.onTogglePause = [&]() {
    audioTogglePause();
  };
  callbacks.onStopPlayback = [&]() {
    if (audioIsReady()) {
      audioStop();
    }
  };
  callbacks.onCurrentPlaybackFile = [&]() { return audioGetNowPlaying(); };
  callbacks.onToggleRadio = [&]() {
    audioToggleRadio();
  };
  callbacks.onToggle50Hz = [&]() {
    if (audioSupports50HzToggle()) {
      audioToggle50Hz();
    }
  };
  callbacks.onToggleOptions = [&]() {
    if (optionsBrowserCanToggle(browser)) {
      optionsBrowserToggle(browser);
      refreshBrowser(browser, "");
    }
  };
  callbacks.onSeekBy = [&](int direction) {
    audioSeekBy(direction);
    markSeekHold(audioGetSeekTargetSec());
  };
  callbacks.onSeekToRatio = [&](double ratio) {
    audioSeekToRatio(ratio);
    markSeekHold(audioGetSeekTargetSec());
  };
  callbacks.onAdjustVolume = [&](float delta) { audioAdjustVolume(delta); };
  callbacks.onToggleMelodyVisualization = [&]() {
    melodyVisualizationEnabled = !melodyVisualizationEnabled;
    if (melodyVisualizationEnabled) {
      browser.filterActive = false;
      breadcrumbHover = -1;
      actionHover = -1;
      clearMelodyHistory();
    }
    fileContextMenu.active = false;
    dirty = true;
  };
  callbacks.onResize = [&]() { rebuildLayout(); };

  bool paletteActive = false;
  std::string paletteQuery;
  int paletteSelected = 0;
  int paletteScroll = 0;
  std::vector<int> paletteFiltered;
  PaletteLayout paletteLayout;

  auto buildCommands = [&]() {
    std::vector<CommandEntry> cmds;
    cmds.push_back({"Play/Pause", "Space", true, [&]() {
                      audioTogglePause();
                      dirty = true;
                    }});
    cmds.push_back(
        {melodyVisualizationEnabled ? "Melody Viz: On" : "Melody Viz: Off",
         "M", true, [&]() {
           melodyVisualizationEnabled = !melodyVisualizationEnabled;
           if (melodyVisualizationEnabled) {
             browser.filterActive = false;
             breadcrumbHover = -1;
             actionHover = -1;
             clearMelodyHistory();
           }
           fileContextMenu.active = false;
           dirty = true;
         }});
    cmds.push_back({audioIsRadioEnabled() ? "Radio Filter: AM"
                                          : "Radio Filter: Dry",
                    "R", true, [&]() {
                      audioToggleRadio();
                      dirty = true;
                    }});
    bool show50Hz = audioSupports50HzToggle();
    if (show50Hz) {
      cmds.push_back({audioIs50HzEnabled() ? "50Hz: 50" : "50Hz: Auto",
                      "H", true, [&]() {
                        audioToggle50Hz();
                        dirty = true;
                      }});
    }
    if (!melodyVisualizationEnabled) {
      cmds.push_back({"View: Grid", "T", true, [&]() {
                        browser.viewMode = BrowserState::ViewMode::Thumbnails;
                        dirty = true;
                      }});
      cmds.push_back({"View: List", "T", true, [&]() {
                        browser.viewMode = BrowserState::ViewMode::ListOnly;
                        dirty = true;
                      }});
      cmds.push_back({"View: Preview", "T", true, [&]() {
                        browser.viewMode = BrowserState::ViewMode::ListPreview;
                        dirty = true;
                      }});
      if (optionsBrowserCanToggle(browser)) {
        cmds.push_back({"Options", "O", true, [&]() {
                          optionsBrowserToggle(browser);
                          refreshBrowser(browser, "");
                          dirty = true;
                        }});
      }
    }
    cmds.push_back({"Quit", "Q", true, [&]() {
                      running = false;
                    }});
    return cmds;
  };

  auto filterCommands = [&](const std::vector<CommandEntry>& cmds,
                            std::vector<int>* out) {
    out->clear();
    std::string q = toLower(paletteQuery);
    auto fuzzyMatch = [](const std::string& text,
                         const std::string& query) {
      if (query.empty()) return true;
      size_t ti = 0;
      for (char qc : query) {
        ti = text.find(qc, ti);
        if (ti == std::string::npos) return false;
        ++ti;
      }
      return true;
    };
    for (int i = 0; i < static_cast<int>(cmds.size()); ++i) {
      if (!cmds[static_cast<size_t>(i)].enabled) continue;
      if (q.empty()) {
        out->push_back(i);
        continue;
      }
      std::string labelLower = toLower(cmds[static_cast<size_t>(i)].label);
      if (fuzzyMatch(labelLower, q)) {
        out->push_back(i);
      }
    }
  };

  auto computePaletteLayout = [&](int w, int h, int listRows) {
    PaletteLayout layout{};
    int maxWidth = std::max(30, w - 4);
    layout.width = std::min(72, maxWidth);
    layout.innerWidth = std::max(1, layout.width - 2);
    layout.listRows = std::max(1, listRows);
    layout.height = layout.listRows + 3;
    layout.x = std::max(0, (w - layout.width) / 2);
    layout.y = std::max(1, (h - layout.height) / 2);
    layout.inputY = layout.y + 1;
    layout.listY = layout.y + 2;
    layout.valid = true;
    return layout;
  };

  auto ensurePaletteScroll = [&](int count, int visibleRows) {
    if (count <= 0) {
      paletteSelected = 0;
      paletteScroll = 0;
      return;
    }
    paletteSelected =
        std::clamp(paletteSelected, 0, std::max(0, count - 1));
    if (paletteSelected < paletteScroll) {
      paletteScroll = paletteSelected;
    } else if (paletteSelected >= paletteScroll + visibleRows) {
      paletteScroll = paletteSelected - visibleRows + 1;
    }
    paletteScroll =
        std::clamp(paletteScroll, 0, std::max(0, count - visibleRows));
  };

  auto buildMelodyOutputPath = [&](const FileEntry& entry) {
    std::filesystem::path output = entry.path;
    int trackIndex = entry.trackIndex;
    if (trackIndex >= 0) {
      char suffix[32];
      std::snprintf(suffix, sizeof(suffix), ".track%03d.melody", trackIndex);
      output += suffix;
      return output;
    }
    output.replace_extension(".melody");
    return output;
  };

  auto cleanupMelodyExportWorker = [&]() {
    bool shouldJoin = false;
    {
      std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
      shouldJoin =
          !melodyExportTask.running && melodyExportTask.worker.joinable();
    }
    if (shouldJoin) {
      melodyExportTask.worker.join();
    }
  };

  auto joinMelodyExportWorker = [&]() {
    if (melodyExportTask.worker.joinable()) {
      melodyExportTask.worker.join();
    }
  };

  auto startMelodyExport = [&](const FileEntry& entry) {
    if (entry.isDir || !isSupportedAudioExt(entry.path)) {
      return;
    }
    if (isBackgroundTaskRunning()) {
      return;
    }

    cleanupMelodyExportWorker();

    int trackIndex = entry.trackIndex >= 0 ? entry.trackIndex : 0;
    std::filesystem::path outputFile = buildMelodyOutputPath(entry);

    {
      std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
      if (melodyExportTask.running) {
        return;
      }
      melodyExportTask.running = true;
      melodyExportTask.hasResult = false;
      melodyExportTask.success = false;
      melodyExportTask.progress = 0.0f;
      melodyExportTask.status.clear();
      melodyExportTask.sourceFile = entry.path;
      melodyExportTask.outputFile = outputFile;
    }

    melodyExportTask.worker =
        std::thread([entryPath = entry.path, trackIndex, outputFile,
                     &melodyExportTask]() {
          auto onProgress = [&melodyExportTask](float progress) {
            std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
            melodyExportTask.progress = std::clamp(progress, 0.0f, 1.0f);
          };
          std::string error;
          bool ok = audioAnalyzeFileToMelodyFile(entryPath, trackIndex,
                                                 outputFile, onProgress, &error);
          std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
          melodyExportTask.running = false;
          melodyExportTask.hasResult = true;
          melodyExportTask.success = ok;
          melodyExportTask.progress = ok ? 1.0f : melodyExportTask.progress;
          if (ok) {
            std::filesystem::path midiOutput = outputFile;
            midiOutput.replace_extension(".mid");
            melodyExportTask.status =
                "Saved " + toUtf8String(outputFile.filename()) + " and " +
                midiOutput.filename().string();
          } else {
            melodyExportTask.status =
                error.empty() ? "Analysis failed." : error;
          }
        });
  };

  auto computeFileContextLayout = [&](int w, int h) {
    FileContextMenuLayout layout{};
    std::vector<std::string> items = {" Play", " Analyze", " Split Loop"};
    int itemWidth = 0;
    for (const auto& item : items) {
      itemWidth = std::max(itemWidth, utf8CodepointCount(item));
    }
    layout.width = std::max(24, itemWidth + 4);
    layout.height = static_cast<int>(items.size()) + 2;
    int x = fileContextMenu.anchorX;
    int y = fileContextMenu.anchorY;
    if (x < 0 || y < 0) {
      x = (w - layout.width) / 2;
      y = (h - layout.height) / 2;
    }
    x = std::clamp(x, 0, std::max(0, w - layout.width));
    y = std::clamp(y, 1, std::max(1, h - layout.height));
    layout.x = x;
    layout.y = y;
    layout.listY = y + 1;
    layout.rows = static_cast<int>(items.size());
    layout.valid = true;
    return layout;
  };

  auto runFileContextAction = [&](int actionIndex) {
    if (!fileContextMenu.active) return;
    const FileEntry entry = fileContextMenu.entry;
    if (actionIndex == 0) {
      bool played = false;
      if (entry.trackIndex >= 0) {
        played = audioStartFile(entry.path, entry.trackIndex);
      } else if (callbacks.onPlayFile) {
        played = callbacks.onPlayFile(entry.path);
      }
      (void)played;
    } else if (actionIndex == 1) {
      startMelodyExport(entry);
    } else if (actionIndex == 2) {
      if (!entry.isDir && isSupportedAudioExt(entry.path) &&
          !isBackgroundTaskRunning()) {
        LoopSplitConfig splitConfig;
        splitConfig.channels = 2;
        splitConfig.sampleRate = 48000;
        splitConfig.trackIndex = std::max(0, entry.trackIndex);
        splitConfig.kssOptions = audioGetKssOptionState();
        splitConfig.nsfOptions = audioGetNsfOptionState();
        splitConfig.vgmOptions = audioGetVgmOptionState();
        startLoopSplitExport(entry.path, o.output, splitConfig, loopSplitTask);
      }
    }
    fileContextMenu.active = false;
    dirty = true;
  };

  auto drawMelodyPanel = [&](int top, int panelHeight, int panelWidth,
                             const AudioMelodyInfo& melodyInfo,
                             const AudioMelodyAnalysisState& melodyAnalysisState) {
    if (panelHeight <= 0 || panelWidth <= 0) return;
    int bottom = top + panelHeight;
    int row = top;

    if (row < bottom) {
      screen.writeText(0, row++, fitLine(" Melody", panelWidth), kStyleAccent);
    }

    if (row < bottom) {
      std::string noteLine;
      if (melodyInfo.midiNote >= 0) {
        int hz = static_cast<int>(std::round(melodyInfo.frequencyHz));
        noteLine = " " + midiToNoteName(melodyInfo.midiNote) + "   " +
                   std::to_string(hz) + "Hz";
      } else {
        noteLine = " --";
      }
      screen.writeText(0, row++, fitLine(noteLine, panelWidth), kStyleNormal);
    }

    if (row < bottom) {
      int pct =
          static_cast<int>(std::round(std::clamp(melodyInfo.confidence, 0.0f, 1.0f) *
                                      100.0f));
      int meterWidth = std::max(8, panelWidth - 20);
      int filled =
          static_cast<int>(std::round(static_cast<float>(meterWidth) *
                                      std::clamp(melodyInfo.confidence, 0.0f, 1.0f)));
      filled = std::clamp(filled, 0, meterWidth);
      std::string meter = "[";
      meter.append(static_cast<size_t>(filled), '#');
      meter.append(static_cast<size_t>(std::max(0, meterWidth - filled)), '.');
      meter.push_back(']');
      std::string confLine = " Confidence " + meter + " " + std::to_string(pct) + "%";
      screen.writeText(0, row++, fitLine(confLine, panelWidth), kStyleDim);
    }

    if (row < bottom) {
      std::string statusLine;
      if (!melodyAnalysisState.error.empty()) {
        statusLine = " Analysis: Error - " + melodyAnalysisState.error;
      } else if (melodyAnalysisState.ready) {
        statusLine =
            " Analysis: Ready (" + std::to_string(melodyAnalysisState.frameCount) +
            " pts)";
      } else if (melodyAnalysisState.running) {
        int progressPercent =
            static_cast<int>(std::round(std::clamp(melodyAnalysisState.progress,
                                                   0.0f, 1.0f) *
                                      100.0f));
        statusLine = " Analysis: " + std::to_string(progressPercent) + "%";
      } else {
        statusLine = " Analysis: Idle";
      }
      screen.writeText(0, row++, fitLine(statusLine, panelWidth), kStyleDim);
    }

    int graphTop = row;
    int graphHeight = bottom - graphTop;
    if (graphHeight < 4) return;

    constexpr int kGraphMinMidi = 36;  // C2
    constexpr int kGraphMaxMidi = 96;  // C7
    int labelWidth = (panelWidth >= 34) ? 7 : 0;
    int chartX = labelWidth;
    int chartWidth = panelWidth - chartX;
    if (chartWidth < 8) return;

    auto midiToRow = [&](int midi) {
      int clamped = std::clamp(midi, kGraphMinMidi, kGraphMaxMidi);
      int range = kGraphMaxMidi - kGraphMinMidi;
      if (range <= 0 || graphHeight <= 1) return graphTop;
      float norm =
          static_cast<float>(kGraphMaxMidi - clamped) / static_cast<float>(range);
      int offset = static_cast<int>(std::round(norm * (graphHeight - 1)));
      return graphTop + std::clamp(offset, 0, graphHeight - 1);
    };

    for (int midi = kGraphMinMidi; midi <= kGraphMaxMidi; midi += 12) {
      int y = midiToRow(midi);
      if (y < graphTop || y >= bottom) continue;
      if (labelWidth > 0) {
        std::string label = midiToNoteName(midi);
        if (utf8CodepointCount(label) < labelWidth) {
          label.insert(label.begin(),
                       static_cast<size_t>(labelWidth - utf8CodepointCount(label)),
                       ' ');
        }
        screen.writeText(0, y, utf8Take(label, labelWidth), kStyleDim);
      }
      screen.writeRun(chartX, y, chartWidth, L'.', kStyleDim);
    }

    size_t sampleCount = melodyHistoryMidi.size();
    if (sampleCount > 0) {
      size_t start = 0;
      if (sampleCount > static_cast<size_t>(chartWidth)) {
        start = sampleCount - static_cast<size_t>(chartWidth);
      }
      for (int x = 0; x < chartWidth; ++x) {
        size_t idx = start + static_cast<size_t>(x);
        if (idx >= sampleCount) break;
        int midi = melodyHistoryMidi[idx];
        if (midi < 0) continue;
        int y = midiToRow(midi);
        float conf = melodyHistoryConfidence[idx];
        Style pointStyle = kStyleDim;
        wchar_t point = L'*';
        if (conf >= 0.75f) {
          pointStyle = kStyleAccent;
          point = L'#';
        } else if (conf >= 0.45f) {
          pointStyle = kStyleNormal;
        }
        screen.writeChar(chartX + x, y, point, pointStyle);
      }
    }

    if (melodyInfo.midiNote >= 0) {
      int y = midiToRow(melodyInfo.midiNote);
      int x = chartX + chartWidth - 1;
      if (x >= chartX && y >= graphTop && y < bottom) {
        screen.writeChar(x, y, L'@', kStyleAccent);
      }
    }
  };

  while (running) {
    rebuildLayout();
    cleanupMelodyExportWorker();
    cleanupLoopSplitExportWorker(loopSplitTask);

    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      tuiWindow.PollEvents();
    }

    auto processInputEvent = [&](InputEvent ev) {
      if (ev.type == InputEvent::Type::Mouse &&
          (ev.mouse.control & 0x80000000) != 0) {
        int wndW = std::max(1, tuiWindow.GetWidth());
        int wndH = std::max(1, tuiWindow.GetHeight());
        int gridW = std::max(1, screen.width());
        int gridH = std::max(1, screen.height());
        int gx = static_cast<int>((static_cast<int64_t>(ev.mouse.pos.X) * gridW) /
                                  wndW);
        int gy = static_cast<int>((static_cast<int64_t>(ev.mouse.pos.Y) * gridH) /
                                  wndH);
        gx = std::clamp(gx, 0, gridW - 1);
        gy = std::clamp(gy, 0, gridH - 1);
        ev.mouse.pos.X = static_cast<SHORT>(gx);
        ev.mouse.pos.Y = static_cast<SHORT>(gy);
      }
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        bool paletteToggle =
            ctrl && (key.vk == 'P' || key.ch == 'p' || key.ch == 'P');
        if (paletteToggle) {
          paletteActive = !paletteActive;
          if (paletteActive) {
            fileContextMenu.active = false;
          }
          paletteQuery.clear();
          paletteSelected = 0;
          paletteScroll = 0;
          dirty = true;
          return;
        }
      }
      if (fileContextMenu.active) {
        fileContextLayout = computeFileContextLayout(width, height);
        if (ev.type == InputEvent::Type::Key) {
          const KeyEvent& key = ev.key;
          if (key.vk == VK_ESCAPE) {
            fileContextMenu.active = false;
            dirty = true;
            return;
          }
          if (key.vk == VK_UP) {
            fileContextMenu.selected =
                (fileContextMenu.selected + fileContextLayout.rows - 1) %
                fileContextLayout.rows;
            dirty = true;
            return;
          }
          if (key.vk == VK_DOWN) {
            fileContextMenu.selected =
                (fileContextMenu.selected + 1) % fileContextLayout.rows;
            dirty = true;
            return;
          }
          if (key.vk == VK_RETURN) {
            runFileContextAction(fileContextMenu.selected);
            return;
          }
          return;
        }
        if (ev.type == InputEvent::Type::Mouse) {
          const MouseEvent& mouse = ev.mouse;
          bool leftPressed =
              (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
          if (mouse.eventFlags == MOUSE_WHEELED) {
            int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
            if (delta != 0) {
              if (delta > 0) {
                fileContextMenu.selected =
                    (fileContextMenu.selected + fileContextLayout.rows - 1) %
                    fileContextLayout.rows;
              } else {
                fileContextMenu.selected =
                    (fileContextMenu.selected + 1) % fileContextLayout.rows;
              }
              dirty = true;
            }
            return;
          }
          if (mouse.eventFlags == 0 && leftPressed) {
            bool inside =
                fileContextLayout.valid &&
                mouse.pos.X >= fileContextLayout.x &&
                mouse.pos.X < fileContextLayout.x + fileContextLayout.width &&
                mouse.pos.Y >= fileContextLayout.y &&
                mouse.pos.Y < fileContextLayout.y + fileContextLayout.height;
            if (inside && mouse.pos.Y >= fileContextLayout.listY &&
                mouse.pos.Y < fileContextLayout.listY + fileContextLayout.rows) {
              int action = mouse.pos.Y - fileContextLayout.listY;
              if (action >= 0 && action < fileContextLayout.rows) {
                fileContextMenu.selected = action;
                runFileContextAction(action);
                return;
              }
            }
            fileContextMenu.active = false;
            dirty = true;
            return;
          }
          return;
        }
      }
      if (paletteActive) {
        auto cmds = buildCommands();
        filterCommands(cmds, &paletteFiltered);
        int maxRows = std::max(1, height - 8);
        int visibleRows =
            std::min(maxRows, std::max(1, static_cast<int>(paletteFiltered.size())));
        ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                            visibleRows);
        if (ev.type == InputEvent::Type::Key) {
          const KeyEvent& key = ev.key;
          const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
          const DWORD altMask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
          bool ctrl = (key.control & ctrlMask) != 0;
          bool alt = (key.control & altMask) != 0;
          if (key.vk == VK_ESCAPE) {
            paletteActive = false;
            dirty = true;
            return;
          }
          if (key.vk == VK_RETURN) {
            if (!paletteFiltered.empty()) {
              int idx = paletteFiltered[static_cast<size_t>(
                  std::clamp(paletteSelected, 0,
                             static_cast<int>(paletteFiltered.size()) - 1))];
              if (idx >= 0 && idx < static_cast<int>(cmds.size())) {
                cmds[static_cast<size_t>(idx)].run();
              }
            }
            paletteActive = false;
            dirty = true;
            return;
          }
          if (key.vk == VK_UP) {
            paletteSelected--;
            ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                                visibleRows);
            dirty = true;
            return;
          }
          if (key.vk == VK_DOWN) {
            paletteSelected++;
            ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                                visibleRows);
            dirty = true;
            return;
          }
          if (key.vk == VK_BACK) {
            if (!paletteQuery.empty()) {
              paletteQuery.pop_back();
              paletteSelected = 0;
              paletteScroll = 0;
            }
            dirty = true;
            return;
          }
          if (!ctrl && !alt && key.ch >= 32) {
            paletteQuery.push_back(key.ch);
            paletteSelected = 0;
            paletteScroll = 0;
            dirty = true;
            return;
          }
          return;
        }
        if (ev.type == InputEvent::Type::Mouse) {
          const MouseEvent& mouse = ev.mouse;
          bool leftPressed =
              (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
          if (mouse.eventFlags == MOUSE_WHEELED) {
            int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
            if (delta != 0) {
              paletteSelected -= delta / WHEEL_DELTA;
              ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                                  visibleRows);
              dirty = true;
            }
            return;
          }
          if (leftPressed && mouse.eventFlags == 0) {
            paletteLayout = computePaletteLayout(width, height, visibleRows);
            if (paletteLayout.valid) {
              if (mouse.pos.Y >= paletteLayout.listY &&
                  mouse.pos.Y < paletteLayout.listY + paletteLayout.listRows &&
                  mouse.pos.X >= paletteLayout.x + 1 &&
                  mouse.pos.X < paletteLayout.x + paletteLayout.width - 1) {
                int rel = mouse.pos.Y - paletteLayout.listY;
                int idx = paletteScroll + rel;
                if (idx >= 0 &&
                    idx < static_cast<int>(paletteFiltered.size())) {
                  int cmdIndex =
                      paletteFiltered[static_cast<size_t>(idx)];
                  if (cmdIndex >= 0 &&
                      cmdIndex < static_cast<int>(cmds.size())) {
                    cmds[static_cast<size_t>(cmdIndex)].run();
                  }
                  paletteActive = false;
                  dirty = true;
                  return;
                }
              }
            }
          }
          return;
        }
      }
      bool browserInteractionEnabled = !melodyVisualizationEnabled;
      handleInputEvent(ev, browser, layout, breadcrumbLine, breadcrumbY,
                       listTop, listHeight, progressBarX, progressBarY,
                       progressBarWidth, actionStrip, browserInteractionEnabled,
                       o.play, audioIsReady(), breadcrumbHover, actionHover,
                       dirty, running, callbacks);
    };

    auto finalizeRenderedExit = [&]() {
      if (windowTuiEnabled && tuiWindow.IsOpen()) {
        tuiWindow.Close();
      }
      joinMelodyExportWorker();
      joinLoopSplitExportWorker(loopSplitTask);
      audioShutdown();
    };

    InputEvent ev{};
    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      while (running && tuiWindow.PollInput(ev)) {
        processInputEvent(ev);
        if (rendered) {
          finalizeRenderedExit();
          return 0;
        }
      }
    }

    while (running && input.poll(ev)) {
      processInputEvent(ev);
      if (rendered) {
        finalizeRenderedExit();
        return 0;
      }
      if (!running) break;
    }
    if (rendered) {
      finalizeRenderedExit();
      return 0;
    }
    if (!running) break;

    auto now = std::chrono::steady_clock::now();
    if (dirty || now - lastDraw >= std::chrono::milliseconds(150)) {
      screen.updateSize();
      width = std::max(40, screen.width());
      height = std::max(10, screen.height());
      bool optionsMode = optionsBrowserIsActive(browser);
      bool trackMode = isTrackBrowserActive(browser);
      bool browserInteractionEnabled = !melodyVisualizationEnabled;
      bool showHeaderLabel =
          browserInteractionEnabled && (optionsMode || trackMode);
      headerLines = showHeaderLabel ? 2 : 1;
      if (browserInteractionEnabled) {
        listTop = headerLines + 1;
        breadcrumbY = listTop - 1;
      } else {
        listTop = headerLines;
        breadcrumbY = -1;
      }
      listHeight = height - listTop - footerLines;
      if (listHeight < 1) listHeight = 1;
      layout = buildLayout(browser, width, listHeight);
      if (layout.totalRows <= layout.rowsVisible) {
        browser.scrollRow = 0;
      } else {
        int maxScroll = layout.totalRows - layout.rowsVisible;
        browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
      }

      screen.clear(kStyleNormal);

      const std::string titleRaw = "Radioify";
      std::string title = titleRaw;
      if (static_cast<int>(title.size()) > width) {
        title = fitLine(titleRaw, width);
      }
      int titleLen = static_cast<int>(title.size());
      int titleX = std::max(0, (width - titleLen) / 2);
      double seconds =
          std::chrono::duration<double>(now.time_since_epoch()).count();
      double speed = 1.3;
      float pulse = static_cast<float>(0.5 * (std::sin(seconds * speed) + 1.0));
      pulse = clamp01(pulse);
      pulse = pulse * pulse * (3.0f - 2.0f * pulse);
      float t = std::pow(pulse, 0.6f);
      float flash = 0.0f;
      if (t > 0.88f) {
        flash = (t - 0.88f) / 0.12f;
        flash = flash * flash;
      }
      Color headerBg = lerpColor(kStyleHeader.bg, kStyleHeaderHot.bg,
                                 std::min(0.85f, t * 0.9f));
      headerBg = lerpColor(headerBg, Color{52, 44, 26}, flash * 0.7f);
      Style headerLineStyle{kStyleHeader.fg, headerBg};
      screen.writeRun(0, 0, width, L' ', headerLineStyle);

      Color titleFg;
      if (t < 0.35f) {
        titleFg = lerpColor(kStyleHeader.fg, kStyleHeaderGlow.fg, t / 0.35f);
      } else {
        float hotT = (t - 0.35f) / 0.65f;
        titleFg =
            lerpColor(kStyleHeaderGlow.fg, kStyleHeaderHot.fg, clamp01(hotT));
      }
      if (t > 0.85f) {
        float whiteT = (t - 0.85f) / 0.15f;
        titleFg = lerpColor(titleFg, Color{255, 255, 255}, clamp01(whiteT));
      }
      if (flash > 0.0f) {
        titleFg = lerpColor(titleFg, Color{255, 236, 186}, clamp01(flash));
      }
      Style titleAttr{titleFg, headerBg};
      for (int i = 0; i < titleLen; ++i) {
        wchar_t ch = static_cast<wchar_t>(
            static_cast<unsigned char>(title[static_cast<size_t>(i)]));
        screen.writeChar(titleX + i, 0, ch, titleAttr);
      }
      breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
      if (browserInteractionEnabled) {
        if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
          breadcrumbHover = -1;
        }
        screen.writeText(0, breadcrumbY, breadcrumbLine.text, kStyleAccent);
        if (breadcrumbHover >= 0) {
          const auto& crumb =
              breadcrumbLine.crumbs[static_cast<size_t>(breadcrumbHover)];
          std::string hoverText = utf8Slice(breadcrumbLine.text, crumb.startX,
                                            crumb.endX - crumb.startX);
          screen.writeText(crumb.startX, breadcrumbY, hoverText,
                           kStyleBreadcrumbHover);
        }
      } else {
        breadcrumbHover = -1;
      }
      std::filesystem::path nowPlaying = audioGetNowPlaying();
      int nowPlayingTrackIndex = audioGetTrackIndex();
      if (nowPlaying != lastMelodyTrack ||
          nowPlayingTrackIndex != lastMelodyTrackIndex) {
        clearMelodyHistory();
        lastMelodyTrack = nowPlaying;
        lastMelodyTrackIndex = nowPlayingTrackIndex;
      }
      std::string showingLabel;
      if (!browserInteractionEnabled) {
        showingLabel.clear();
      } else if (optionsMode) {
        showingLabel = optionsBrowserShowingLabel();
      } else if (trackMode) {
        showingLabel =
            "  Showing: tracks in " + toUtf8String(browser.dir.filename());
      } else {
        showingLabel.clear();
      }
      if (!showingLabel.empty()) {
        screen.writeText(0, 1, fitLine(showingLabel, width), kStyleDim);
      }
      AudioMelodyInfo melodyInfo = audioGetMelodyInfo();
      AudioMelodyAnalysisState melodyAnalysisState = audioGetMelodyAnalysisState();
      if (melodyAnalysisState.running && !lastMelodyAnalysisRunning) {
        clearMelodyHistory();
      }
      lastMelodyAnalysisRunning = melodyAnalysisState.running;
      if (melodyVisualizationEnabled && !audioIsPaused() && !audioIsHolding()) {
        pushMelodyHistory(melodyInfo);
      }
      if (browserInteractionEnabled) {
        drawBrowserEntries(screen, browser, layout, listTop, listHeight,
                           kStyleNormal, kStyleNormal, kStyleDir,
                           kStyleHighlight, kStyleDim, isSupportedImageExt,
                           isVideoExt, isSupportedAudioExt);
      } else {
        drawMelodyPanel(listTop, listHeight, width, melodyInfo,
                        melodyAnalysisState);
      }

      int footerStart = listTop + listHeight;
      int line = footerStart;
      if (line < height && browserInteractionEnabled) {
        std::string meta;
        if (optionsMode) {
          meta = optionsBrowserSelectionMeta(browser);
        } else if (trackMode) {
          meta = buildTrackSelectionMeta(browser);
        } else {
          meta = buildSelectionMeta(browser, isVideoExt);
        }
        if (!meta.empty()) {
          screen.writeText(0, line++, fitLine(meta, width), kStyleDim);
        }
      }
      if (line < height) {
        std::string warning = audioGetWarning();
        if (!warning.empty()) {
          screen.writeText(0, line++, fitLine("  Warning: " + warning, width),
                           kStyleDim);
        }
      }
      if (line < height) {
        bool exportRunning = false;
        bool exportHasResult = false;
        bool exportSuccess = false;
        std::string exportStatus;
        {
          std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
          exportRunning = melodyExportTask.running;
          exportHasResult = melodyExportTask.hasResult;
          exportSuccess = melodyExportTask.success;
          exportStatus = melodyExportTask.status;
        }
        if (exportHasResult && !exportStatus.empty()) {
          Style statusStyle = exportSuccess ? kStyleDim : kStyleAlert;
          screen.writeText(0, line++,
                           fitLine(" Analyze: " + exportStatus, width),
                           statusStyle);
        }
        bool loopSplitHasResult = false;
        bool loopSplitSuccess = false;
        std::string loopSplitStatus;
        {
          std::lock_guard<std::mutex> lock(loopSplitTask.mutex);
          loopSplitHasResult = loopSplitTask.hasResult;
          loopSplitSuccess = loopSplitTask.success;
          loopSplitStatus = loopSplitTask.status;
        }
        if (loopSplitHasResult && !loopSplitStatus.empty()) {
          Style statusStyle = loopSplitSuccess ? kStyleDim : kStyleAlert;
          screen.writeText(0, line++,
                           fitLine(" Loop Split: " + loopSplitStatus, width),
                           statusStyle);
        }
      }
      std::string nowLabel =
          nowPlaying.empty() ? "(none)" : toUtf8String(nowPlaying.filename());
      int trackIndex = audioGetTrackIndex();
      if (!nowPlaying.empty() && trackIndex >= 0) {
        int digits = 3;
        const TrackEntry* track = nullptr;
        TrackEntry fallback{};
        if (nowPlaying == gTrackBrowser.file && !gTrackBrowser.tracks.empty()) {
          digits = trackLabelDigits(gTrackBrowser.tracks.size());
          track = findTrackEntry(trackIndex);
        }
        if (!track) {
          fallback.index = trackIndex;
          track = &fallback;
        }
        nowLabel += "  |  " + formatTrackLabel(*track, digits);
      }
      screen.writeText(0, line++, fitLine(std::string(" ") + nowLabel, width),
                       kStyleAccent);

      actionStrip.buttons.clear();
      actionStrip.y = -1;
      if (o.play && line + 1 < height) {
        actionStrip.y = line;
        struct ActionRenderItem {
          ActionStripItem id;
          std::string label;
          std::string labelHover;
          bool active;
          int width;
        };
        std::vector<ActionRenderItem> items;
        auto makeLabels = [](const std::string& text) {
          return std::pair<std::string, std::string>(" [" + text + "] ",
                                                     "[ " + text + " ]");
        };
        std::string radioState = audioIsRadioEnabled() ? "Radio: AM" : "Radio: Dry";
        auto radioLabels = makeLabels(radioState);
        items.push_back({ActionStripItem::Radio, radioLabels.first,
                         radioLabels.second, audioIsRadioEnabled(),
                         std::max(utf8CodepointCount(radioLabels.first),
                                  utf8CodepointCount(radioLabels.second))});
        auto melodyLabels =
            makeLabels(melodyVisualizationEnabled ? "Melody: On" : "Melody: Off");
        items.push_back(
            {ActionStripItem::MelodyViz, melodyLabels.first,
             melodyLabels.second, melodyVisualizationEnabled,
             std::max(utf8CodepointCount(melodyLabels.first),
                      utf8CodepointCount(melodyLabels.second))});
        bool show50Hz = audioSupports50HzToggle();
        if (browserInteractionEnabled) {
          const std::string gridIcon = "\xE2\x96\xA6";
          const std::string listIcon = "\xE2\x89\xA1";
          const std::string previewIcon = "\xE2\x98\x90";
          std::string viewState;
          switch (browser.viewMode) {
            case BrowserState::ViewMode::Thumbnails:
              viewState = gridIcon + " Grid";
              break;
            case BrowserState::ViewMode::ListOnly:
              viewState = listIcon + " List";
              break;
            case BrowserState::ViewMode::ListPreview:
              viewState = previewIcon + " Preview";
              break;
          }
          auto viewLabels = makeLabels(viewState);
          items.push_back({ActionStripItem::View, viewLabels.first,
                           viewLabels.second, false,
                           std::max(utf8CodepointCount(viewLabels.first),
                                    utf8CodepointCount(viewLabels.second))});
        }
        if (show50Hz) {
          std::string hzState =
              audioIs50HzEnabled() ? "50Hz: 50" : "50Hz: Auto";
          auto hzLabels = makeLabels(hzState);
          items.push_back({ActionStripItem::Hz50, hzLabels.first,
                           hzLabels.second, audioIs50HzEnabled(),
                           std::max(utf8CodepointCount(hzLabels.first),
                                    utf8CodepointCount(hzLabels.second))});
        }
        if (browserInteractionEnabled && optionsBrowserCanToggle(browser)) {
          auto optLabels = makeLabels("Options");
          bool optActive = optionsBrowserIsActive(browser);
          items.push_back({ActionStripItem::Options, optLabels.first,
                           optLabels.second, optActive,
                           std::max(utf8CodepointCount(optLabels.first),
                                    utf8CodepointCount(optLabels.second))});
        }
        const int gap = 2;
        int x = 0;
        for (const auto& item : items) {
          bool hovered = (actionHover == static_cast<int>(actionStrip.buttons.size()));
          std::string text = hovered ? item.labelHover : item.label;
          int textWidth = utf8CodepointCount(text);
          if (x >= width) break;
          int avail = width - x;
          if (avail <= 0) break;
          int widthUsed = std::min(item.width, avail);
          if (textWidth > widthUsed) {
            text = utf8Take(text, widthUsed);
            textWidth = utf8CodepointCount(text);
          }
          if (textWidth <= 0) break;
          if (textWidth < widthUsed) {
            text.append(static_cast<size_t>(widthUsed - textWidth), ' ');
            textWidth = widthUsed;
          }
          ActionStripButton btn{item.id, x, x + widthUsed};
          actionStrip.buttons.push_back(btn);
          Style style = item.active ? kStyleActionActive : kStyleNormal;
          screen.writeText(x, line, text, style);
          x += widthUsed + gap;
        }
        if (actionHover >= static_cast<int>(actionStrip.buttons.size())) {
          actionHover = -1;
        }
        line++;
      } else {
        actionHover = -1;
      }

      int peakMeterY = -1;
      if (line + 1 < height) {
        peakMeterY = line;
        line++;
      }

      bool audioReady = audioIsReady();
      double currentSec = audioReady ? audioGetTimeSec() : 0.0;
      double totalSec = audioReady ? audioGetTotalSec() : -1.0;
      double displaySec = currentSec;
      if (audioReady) {
        if (audioIsSeeking()) {
          double seekSec = audioGetSeekTargetSec();
          if (seekSec >= 0.0) {
            displaySec = seekSec;
            seekDisplaySec = seekSec;
            seekHoldActive = true;
            seekHoldStart = now;
          }
        } else if (seekHoldActive) {
          double diff = std::abs(currentSec - seekDisplaySec);
          auto holdAge = now - seekHoldStart;
          if ((audioIsPrimed() && diff <= 0.25) ||
              holdAge > std::chrono::seconds(2)) {
            seekHoldActive = false;
          } else {
            displaySec = seekDisplaySec;
          }
        }
      } else {
        seekHoldActive = false;
      }
      std::string status;
      if (audioReady) {
        if (audioIsFinished()) {
          status = "\xE2\x96\xA0";  // ended icon
        } else if (audioIsPaused()) {
          status = "\xE2\x8F\xB8";  // paused icon
        } else {
          status = "\xE2\x96\xB6";  // playing icon
        }
      } else {
        status = "\xE2\x97\x8B";  // idle icon
      }
      int volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
      ProgressTextLayout progressText = buildProgressTextLayout(
          displaySec, totalSec, status, volPct, width);
      std::string suffix = progressText.suffix;
      int barWidth = progressText.barWidth;
      double ratio = 0.0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
      }
      progressBarX = 1;
      progressBarY = line;
      progressBarWidth = barWidth;

      screen.writeChar(0, line, L'|', kStyleProgressFrame);
      auto barCells = renderProgressBarCells(
          ratio, barWidth, kStyleProgressEmpty, kProgressStart, kProgressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(1 + i, line, cell.ch, cell.style);
      }
      screen.writeChar(1 + barWidth, line, L'|', kStyleProgressFrame);
      int suffixBaseX = 2 + barWidth;
      std::string rendered;
      if (!suffix.empty()) {
        rendered = " " + suffix;
        screen.writeText(suffixBaseX, line, rendered, kStyleNormal);
      }
      if (peakMeterY >= 0 && !rendered.empty()) {
        float peak = std::clamp(audioGetPeak(), 0.0f, 1.2f);
        int meterWidth = 8;
        size_t volPos = rendered.find(" Vol:");
        if (volPos == std::string::npos) {
          volPos = rendered.find(" V:");
        }
        int meterX = suffixBaseX;
        if (volPos != std::string::npos) {
          int prefixWidth =
              utf8CodepointCount(rendered.substr(0, volPos + 1));
          meterX = suffixBaseX + prefixWidth;
        }
        if (meterX < 0) meterX = 0;
        if (meterX + meterWidth > width) {
          meterWidth = std::max(0, width - meterX);
        }
        if (meterWidth > 0) {
          double meterRatio = std::clamp(static_cast<double>(peak), 0.0, 1.0);
          Color meterStart = kStyleProgressFrame.fg;
          Color meterEnd = kStyleProgressFrame.fg;
          if (peak >= 0.80f) {
            meterStart = kStyleAccent.fg;
            meterEnd = kProgressEnd;
          }
          auto meterCells = renderProgressBarCells(
              meterRatio, meterWidth, kStyleProgressEmpty, meterStart, meterEnd);
          for (int i = 0; i < meterWidth; ++i) {
            const auto& cell = meterCells[static_cast<size_t>(i)];
            screen.writeChar(meterX + i, peakMeterY, cell.ch, cell.style);
          }
        }
      }

      if (paletteActive) {
        auto cmds = buildCommands();
        filterCommands(cmds, &paletteFiltered);
        int maxRows = std::max(1, height - 8);
        int visibleRows =
            std::min(maxRows, std::max(1, static_cast<int>(paletteFiltered.size())));
        ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                            visibleRows);
        paletteLayout = computePaletteLayout(width, height, visibleRows);
        if (paletteLayout.valid) {
          int x0 = paletteLayout.x;
          int y0 = paletteLayout.y;
          int w = paletteLayout.width;
          int h = paletteLayout.height;
          int inner = paletteLayout.innerWidth;

          // Fill background
          for (int y = 0; y < h; ++y) {
            screen.writeRun(x0, y0 + y, w, L' ', kStyleNormal);
          }

          // Border
          screen.writeChar(x0, y0, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0, L'+', kStyleDim);
          screen.writeChar(x0, y0 + h - 1, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0 + h - 1, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0 + h - 1, L'+', kStyleDim);
          for (int y = 1; y < h - 1; ++y) {
            screen.writeChar(x0, y0 + y, L'|', kStyleDim);
            screen.writeChar(x0 + w - 1, y0 + y, L'|', kStyleDim);
          }

          std::string prompt = "> " + paletteQuery;
          if (utf8CodepointCount(prompt) > inner) {
            prompt = utf8Take(prompt, inner);
          }
          screen.writeText(x0 + 1, paletteLayout.inputY, prompt, kStyleNormal);

          if (paletteFiltered.empty()) {
            std::string none = "(no matches)";
            if (utf8CodepointCount(none) > inner) {
              none = utf8Take(none, inner);
            }
            screen.writeText(x0 + 1, paletteLayout.listY, none, kStyleDim);
          } else {
            for (int row = 0; row < paletteLayout.listRows; ++row) {
              int idx = paletteScroll + row;
              if (idx < 0 ||
                  idx >= static_cast<int>(paletteFiltered.size())) {
                break;
              }
              const auto& cmd =
                  cmds[static_cast<size_t>(paletteFiltered[static_cast<size_t>(idx)])];
              std::string left = cmd.label;
              std::string right = cmd.hotkey;
              int rightWidth = utf8CodepointCount(right);
              int gap = rightWidth > 0 ? 1 : 0;
              int maxLeft = std::max(0, inner - rightWidth - gap);
              if (utf8CodepointCount(left) > maxLeft) {
                left = utf8Take(left, maxLeft);
              }
              int leftWidth = utf8CodepointCount(left);
              std::string lineText = left;
              if (leftWidth < maxLeft) {
                lineText.append(static_cast<size_t>(maxLeft - leftWidth), ' ');
              }
              if (rightWidth > 0) {
                lineText.push_back(' ');
                lineText += right;
              }
              Style lineStyle =
                  (idx == paletteSelected) ? kStyleHighlight : kStyleNormal;
              screen.writeText(x0 + 1, paletteLayout.listY + row, lineText,
                               lineStyle);
            }
          }
        }
      } else {
        paletteLayout.valid = false;
      }

      if (fileContextMenu.active) {
        fileContextLayout = computeFileContextLayout(width, height);
        if (fileContextLayout.valid) {
          int x0 = fileContextLayout.x;
          int y0 = fileContextLayout.y;
          int w = fileContextLayout.width;
          int h = fileContextLayout.height;

          for (int y = 0; y < h; ++y) {
            screen.writeRun(x0, y0 + y, w, L' ', kStyleNormal);
          }

          screen.writeChar(x0, y0, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0, L'+', kStyleDim);
          screen.writeChar(x0, y0 + h - 1, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0 + h - 1, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0 + h - 1, L'+', kStyleDim);
          for (int y = 1; y < h - 1; ++y) {
            screen.writeChar(x0, y0 + y, L'|', kStyleDim);
            screen.writeChar(x0 + w - 1, y0 + y, L'|', kStyleDim);
          }

          std::vector<std::string> items = {" Play", " Analyze", " Split Loop"};
          int inner = std::max(1, w - 2);
          for (int i = 0; i < fileContextLayout.rows; ++i) {
            if (i < 0 || i >= static_cast<int>(items.size())) {
              continue;
            }
            std::string text = items[static_cast<size_t>(i)];
            if (utf8CodepointCount(text) > inner) {
              text = utf8Take(text, inner);
            }
            int textWidth = utf8CodepointCount(text);
            if (textWidth < inner) {
              text.append(static_cast<size_t>(inner - textWidth), ' ');
            }
            Style rowStyle =
                (i == fileContextMenu.selected) ? kStyleHighlight : kStyleNormal;
            screen.writeText(x0 + 1, fileContextLayout.listY + i, text, rowStyle);
          }
        }
      } else {
        fileContextLayout.valid = false;
      }

      {
        bool isRunning = false;
        float exportProgress = 0.0f;
        std::filesystem::path exportSource;
        std::string title = " Melody Analysis";
        {
          std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
          if (melodyExportTask.running) {
            isRunning = true;
            exportProgress = std::clamp(melodyExportTask.progress, 0.0f, 1.0f);
            exportSource = melodyExportTask.sourceFile;
            title = " Melody Analysis";
          }
        }
        if (!isRunning) {
          std::lock_guard<std::mutex> lock(loopSplitTask.mutex);
          if (loopSplitTask.running) {
            isRunning = true;
            exportProgress = std::clamp(loopSplitTask.progress, 0.0f, 1.0f);
            exportSource = loopSplitTask.sourceFile;
            title = " Loop Split";
          }
        }
        if (isRunning) {
          std::string sourceName =
              exportSource.empty() ? std::string("(unknown)")
                                   : toUtf8String(exportSource.filename());
          int popupWidth = std::max(46, utf8CodepointCount(sourceName) + 8);
          popupWidth = std::min(width - 2, popupWidth);
          popupWidth = std::max(24, popupWidth);
          int popupHeight = 7;
          int x0 = std::max(0, (width - popupWidth) / 2);
          int y0 = std::max(1, (height - popupHeight) / 2);

          for (int y = 0; y < popupHeight; ++y) {
            screen.writeRun(x0, y0 + y, popupWidth, L' ', kStyleNormal);
          }
          screen.writeChar(x0, y0, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0, popupWidth - 2, L'-', kStyleDim);
          screen.writeChar(x0 + popupWidth - 1, y0, L'+', kStyleDim);
          screen.writeChar(x0, y0 + popupHeight - 1, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0 + popupHeight - 1, popupWidth - 2, L'-',
                          kStyleDim);
          screen.writeChar(x0 + popupWidth - 1, y0 + popupHeight - 1, L'+',
                           kStyleDim);
          for (int y = 1; y < popupHeight - 1; ++y) {
            screen.writeChar(x0, y0 + y, L'|', kStyleDim);
            screen.writeChar(x0 + popupWidth - 1, y0 + y, L'|', kStyleDim);
          }

          screen.writeText(x0 + 1, y0 + 1, fitLine(title, popupWidth - 2),
                           kStyleAccent);
          std::string fileLine = " " + sourceName;
          screen.writeText(x0 + 1, y0 + 2, fitLine(fileLine, popupWidth - 2),
                           kStyleDim);

          int barInner = std::max(8, popupWidth - 8);
          int filled = static_cast<int>(
              std::round(static_cast<float>(barInner) * exportProgress));
          filled = std::clamp(filled, 0, barInner);
          std::string bar = "[";
          bar.append(static_cast<size_t>(filled), '#');
          bar.append(static_cast<size_t>(std::max(0, barInner - filled)), '.');
          bar.push_back(']');
          screen.writeText(x0 + 2, y0 + 4, fitLine(bar, popupWidth - 4),
                           kStyleNormal);

          int pct = static_cast<int>(std::round(exportProgress * 100.0f));
          std::string pctLine = " " + std::to_string(pct) + "%";
          screen.writeText(x0 + 2, y0 + 5, fitLine(pctLine, popupWidth - 4),
                           kStyleDim);
        }
      }

      screen.draw();
      if (windowTuiEnabled && tuiWindow.IsOpen()) {
        int gridW = 0;
        int gridH = 0;
        if (screen.snapshot(windowCells, gridW, gridH)) {
          tuiWindow.PresentTextGrid(windowCells, gridW, gridH, true);
        }
      }
      lastDraw = now;
      dirty = false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Clear screen before exiting
  screen.clear(kStyleNormal);
  screen.draw();
  if (windowTuiEnabled && tuiWindow.IsOpen()) {
    tuiWindow.Close();
  }
  input.restore();
  screen.restore();
  std::cout << "\n";
  joinMelodyExportWorker();
  joinLoopSplitExportWorker(loopSplitTask);
  audioShutdown();
  return 0;
}
