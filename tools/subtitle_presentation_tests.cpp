#include "playback/session/presentation_projector.h"
#include "playback/video/subtitle/ass/bitmap_renderer.h"
#include "runtime_helpers.h"

#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {

bool expect(bool condition, const char *message) {
  if (!condition) std::cerr << "subtitle_presentation_tests: " << message << '\n';
  return condition;
}

PlayerTimelineSnapshot at(int64_t sourceUs, int64_t positionUs = -1) {
  PlayerTimelineSnapshot timeline;
  timeline.positionUs = positionUs < 0 ? sourceUs : positionUs;
  timeline.sourcePositionUs = sourceUs;
  return timeline;
}

playback_screen_renderer::PlaybackScreenModel publishedAt(
    std::shared_ptr<const SubtitleTrack> track, int64_t sourceUs) {
  playback_screen_renderer::PlaybackScreenModel model;
  model.media.timeline = at(sourceUs);
  model.media.durationUs = 20'000'000;
  model.media.hasSubtitles = true;
  model.media.subtitlesEnabled = true;
  model.media.subtitleTrack = std::move(track);
  model.overlay = playback_session::projectPlaybackOverlay(
      playback_session::OverlayProjection{model.media});
  return model;
}

std::shared_ptr<const SubtitleTrack> fixture(bool ass) {
  auto track = std::make_shared<SubtitleTrack>();
  track->label = "Full subtitles";
  const int64_t starts[] = {7'000'000, 10'200'000, 14'790'000};
  const int64_t ends[] = {9'400'000, 13'830'000, 16'340'000};
  const char *texts[] = {"First line", "Second line", "Third line"};
  for (int i = 0; i < 3; ++i) {
    SubtitleCue cue;
    cue.startUs = starts[i];
    cue.endUs = ends[i];
    cue.text = texts[i];
    cue.rawText = texts[i];
    cue.assStyled = ass;
    track->cues.push_back(std::move(cue));
  }
  if (ass) {
    track->assScript = std::make_shared<const std::string>(
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 640\nPlayResY: 360\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
        "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
        "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
        "Alignment, MarginL, MarginR, MarginV, Encoding\n"
        "Style: Default,Arial,30,&H00FFFFFF,&H000000FF,&H00000000,&H00000000,"
        "0,0,0,0,100,100,0,0,1,2,0,2,20,20,20,1\n"
        "[Events]\n"
        "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:07.00,0:00:09.40,Default,,0,0,0,,First line\n"
        "Dialogue: 0,0:00:10.20,0:00:13.83,Default,,0,0,0,,Second line\n"
        "Dialogue: 0,0:00:14.79,0:00:16.34,Default,,0,0,0,,Third line\n");
  }
  return track;
}

bool exercisePublishedRevision(bool ass) {
  const auto track = fixture(ass);
  // The session publishes once. The native presenter subsequently receives
  // new video timestamps without mouse movement or any other UI command.
  const auto published = publishedAt(track, 0);
  std::vector<uint8_t> canvas;
  int mismatches = 0;
  for (int64_t clockUs = 0; clockUs < 17'000'000; clockUs += 41'708) {
    const auto ui = playback_session::projectWindowUiState(published, at(clockUs));
    const SubtitleCue *expected = track->cueAt(clockUs);
    if (ui.subtitleClockUs != clockUs ||
        ui.subtitleCues.size() != (expected ? 1u : 0u) ||
        (expected && ui.subtitleCues.front().text != expected->text)) {
      ++mismatches;
    }
    if (ass) {
      const auto rendered = renderAssSubtitlesToBgraCanvas(
          ui.subtitleAssScript, ui.subtitleAssFonts, ui.subtitleClockUs,
          640, 360, &canvas);
      if ((rendered.status == SubtitleAssRenderStatus::WithGlyph) !=
          (expected != nullptr)) ++mismatches;
    }
  }
  bool ok = expect(mismatches == 0,
                   "subtitles must advance for every presented frame without republishing UI");
  if (mismatches) std::cerr << "  ass=" << ass << " mismatches=" << mismatches << '\n';
  for (const auto &cue : track->cues) {
    const auto before = playback_session::projectWindowUiState(published, at(cue.startUs - 1));
    const auto start = playback_session::projectWindowUiState(published, at(cue.startUs));
    const auto last = playback_session::projectWindowUiState(published, at(cue.endUs - 1));
    const auto end = playback_session::projectWindowUiState(published, at(cue.endUs));
    ok &= expect(before.subtitleCues.empty() && start.subtitleCues.size() == 1 &&
                     last.subtitleCues.size() == 1 && end.subtitleCues.empty(),
                 "a cue must remain visible for its complete half-open interval");
  }
  const auto mapped = playback_session::projectWindowUiState(published, at(11'000'000, 1'000'000));
  ok &= expect(mapped.subtitleCues.size() == 1 &&
                   mapped.subtitleCues.front().text == "Second line" &&
                   mapped.subtitleClockUs == 11'000'000 && mapped.displaySec == 1.0,
               "edited playback must select subtitles using source time");
  auto seeking = at(8'000'000);
  seeking.seekTransitionPending = true;
  const auto hidden = playback_session::projectWindowUiState(published, seeking);
  const auto settled = playback_session::projectWindowUiState(published, at(8'000'000));
  ok &= expect(hidden.subtitleCues.empty() && !hidden.subtitleAssScript &&
                   settled.subtitleCues.size() == 1,
               "seek suppression must follow current transport and clear when the seek settles");
  auto publishedWhileSeeking = published;
  publishedWhileSeeking.media.timeline = seeking;
  playback_session::projectPlaybackTimeline(publishedWhileSeeking.overlay,
                                           publishedWhileSeeking.media, seeking);
  const auto afterSeek = playback_session::projectWindowUiState(
      publishedWhileSeeking, at(8'000'000));
  ok &= expect(afterSeek.subtitleCues.size() == 1,
               "a UI revision published during a seek must not keep subtitles suppressed");
  auto disabled = published;
  disabled.media.subtitlesEnabled = false;
  const auto off = playback_session::projectWindowUiState(disabled, at(8'000'000));
  ok &= expect(off.subtitleCues.empty() && !off.subtitleAssScript,
               "current frame timing must respect disabled subtitles");
  // The same projection feeds the ASCII window before cell composition.
  auto asciiOverlay = published.overlay;
  playback_session::projectPlaybackTimeline(asciiOverlay, published.media, at(11'000'000));
  ok &= expect(asciiOverlay.subtitleText == "Second line" &&
                   asciiOverlay.subtitleClockUs == 11'000'000,
               "the ASCII presentation must follow the current frame too");
  return ok;
}

bool exerciseSnapshotLifetime() {
  const auto dir = std::filesystem::temp_directory_path() /
      ("radioify-subtitle-presentation-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  struct Cleanup {
    std::filesystem::path dir;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
  } cleanup{dir};
  const auto write = [&](const char *text) {
    std::ofstream out(dir / "test.srt");
    out << "1\n00:00:07,000 --> 00:00:09,400\n" << text << '\n';
  };
  write("Before reload");
  SubtitleManager manager;
  manager.loadForVideo(dir / "test.mkv");
  const auto old = manager.activeTrackSnapshot();
  bool ok = expect(old && old == manager.activeTrackSnapshot(),
                   "track snapshots must be shared across ordinary presentation revisions");
  write("After reload");
  manager.loadForVideo(dir / "test.mkv");
  const auto current = manager.activeTrackSnapshot();
  const auto *oldCue = old ? old->cueAt(8'000'000) : nullptr;
  const auto *currentCue = current ? current->cueAt(8'000'000) : nullptr;
  ok &= expect(old && current && old != current &&
                   oldCue && currentCue && oldCue->text == "Before reload" &&
                   currentCue->text == "After reload",
               "published track data must survive reload without mutation");
  {
    std::ofstream out(dir / "test.nl.srt");
    out << "1\n00:00:07,000 --> 00:00:09,400\nAlternate track\n";
  }
  manager.loadForVideo(dir / "test.mkv");
  const auto first = manager.activeTrackSnapshot();
  ok &= expect(manager.cycleLanguage(), "the second subtitle track must be selectable");
  const auto alternate = manager.activeTrackSnapshot();
  ok &= expect(first && alternate && first != alternate &&
                   first->cueAt(8'000'000) &&
                   alternate->cueAt(8'000'000) &&
                   alternate->cueAt(8'000'000)->text != first->cueAt(8'000'000)->text,
               "cycling subtitles must publish the newly selected immutable track");
  ok &= expect(first && manager.selectTrackForFile(first->sourcePath),
               "a sidecar subtitle track must be selectable by file");
  const auto selected = manager.activeTrackSnapshot();
  ok &= expect(first && selected && selected != alternate && selected->cueAt(8'000'000) &&
                   selected->cueAt(8'000'000)->text == first->cueAt(8'000'000)->text,
               "selecting a subtitle file must invalidate the previous track snapshot");
  return ok;
}

bool writeEmbeddedAss(const std::filesystem::path &path,
                      const std::string &header,
                      const std::vector<std::string> &texts) {
  struct Muxer {
    AVFormatContext *format = nullptr;
    AVPacket *packet = av_packet_alloc();
    ~Muxer() {
      av_packet_free(&packet);
      if (format) {
        if (format->pb) avio_closep(&format->pb);
        avformat_free_context(format);
      }
    }
  } muxer;
  const auto filename = toUtf8String(path);
  if (!muxer.packet || avformat_alloc_output_context2(
          &muxer.format, nullptr, "matroska", filename.c_str()) < 0) return false;
  AVStream *stream = avformat_new_stream(muxer.format, nullptr);
  if (!stream) return false;
  stream->time_base = AVRational{1, 1000};
  stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
  stream->codecpar->codec_id = AV_CODEC_ID_ASS;
  stream->codecpar->extradata = static_cast<uint8_t *>(
      av_mallocz(header.size() + AV_INPUT_BUFFER_PADDING_SIZE));
  if (!stream->codecpar->extradata) return false;
  stream->codecpar->extradata_size = static_cast<int>(header.size());
  std::memcpy(stream->codecpar->extradata, header.data(), header.size());
  if (avio_open(&muxer.format->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0 ||
      avformat_write_header(muxer.format, nullptr) < 0) return false;
  for (size_t i = 0; i < texts.size(); ++i) {
    const auto event = std::to_string(i) + ",0,Default,,0,0,0,," + texts[i];
    if (av_new_packet(muxer.packet, static_cast<int>(event.size())) < 0) return false;
    std::memcpy(muxer.packet->data, event.data(), event.size());
    muxer.packet->stream_index = stream->index;
    muxer.packet->pts = muxer.packet->dts = av_rescale_q(
        static_cast<int64_t>(i) * 3000, AVRational{1, 1000}, stream->time_base);
    muxer.packet->duration = av_rescale_q(2000, AVRational{1, 1000}, stream->time_base);
    if (av_interleaved_write_frame(muxer.format, muxer.packet) < 0) return false;
  }
  return av_write_trailer(muxer.format) >= 0;
}

bool exerciseAssGraphics() {
  const auto dir = std::filesystem::temp_directory_path() /
      ("radioify-ass-graphics-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(dir);
  struct Cleanup {
    std::filesystem::path dir;
    ~Cleanup() { std::error_code ec; std::filesystem::remove_all(dir, ec); }
  } cleanup{dir};
  const auto basic = fixture(true);
  const auto header = basic->assScript->substr(0, basic->assScript->find("Dialogue:"));
  // None of these events has visible primary text throughout its lifetime.
  // libass still draws the vector shape, animated text and opaque outline.
  const std::vector<std::string> texts = {
      "{\\an7\\pos(20,20)\\p1}m 0 0 l 100 0 100 50 0 50",
      "{\\an7\\pos(20,20)\\1a&HFF&\\t(0,500,\\1a&H00&)"
      "\\t(1500,2000,\\1a&HFF&)}Animated",
      "{\\an7\\pos(20,20)\\1a&HFF&\\3a&H00&\\bord3}Outline"};
  std::string script = header;
  for (size_t i = 0; i < texts.size(); ++i) {
    script += "Dialogue: 0,0:00:0" + std::to_string(i * 3) +
        ".00,0:00:0" + std::to_string(i * 3 + 2) +
        ".00,Default,,0,0,0,," + texts[i] + '\n';
  }
  const auto reference = std::make_shared<const std::string>(std::move(script));
  const auto media = dir / "graphics.mkv";
  if (!expect(writeEmbeddedAss(media, header, texts), "could not create embedded ASS fixture")) {
    return false;
  }
  bool ok = true;
  for (const auto source : {SubtitleTrack::SourceKind::Embedded,
                            SubtitleTrack::SourceKind::Sidecar}) {
    if (source == SubtitleTrack::SourceKind::Sidecar) {
      std::ofstream out(dir / "graphics.ass", std::ios::binary);
      out << *reference;
    }
    SubtitleManager manager;
    manager.loadForVideo(media);
    const auto track = manager.activeTrackSnapshot();
    if (!expect(track && track->sourceKind == source && track->cues.size() == texts.size(),
                "ASS graphics-only tracks must remain selectable with every event intact")) {
      ok = false;
      continue;
    }
    const auto published = publishedAt(track, 0);
    std::vector<uint8_t> expectedCanvas, actualCanvas;
    for (const int64_t time : {1'000'000, 4'000'000, 7'000'000}) {
      const auto expected = renderAssSubtitlesToBgraCanvas(
          reference, {}, time, 640, 360, &expectedCanvas);
      const auto ui = playback_session::projectWindowUiState(published, at(time));
      const auto actual = renderAssSubtitlesToBgraCanvas(
          ui.subtitleAssScript, ui.subtitleAssFonts, ui.subtitleClockUs,
          640, 360, &actualCanvas);
      ok &= expect(expected.status == SubtitleAssRenderStatus::WithGlyph &&
                       actual.status == expected.status && actualCanvas == expectedCanvas,
                   "imported ASS drawings, animations and outlines must match the original script");
    }
    auto asciiOverlay = published.overlay;
    playback_session::projectPlaybackTimeline(asciiOverlay, published.media, at(1'000'000));
    ok &= expect(asciiOverlay.subtitleText.empty(),
                 "vector drawing commands must not appear as plain subtitle text");
  }
  return ok;
}

size_t assEventCount(const std::string &script) {
  size_t count = 0;
  size_t pos = 0;
  while ((pos = script.find("Dialogue:", pos)) != std::string::npos) {
    ++count;
    pos += 9;
  }
  return count;
}

int compareAssMedia(const std::filesystem::path &media,
                    const std::filesystem::path &referencePath) {
  SubtitleManager manager;
  manager.loadForVideo(media);
  const auto track = manager.activeTrackSnapshot();
  if (!track || !track->assScript) return EXIT_FAILURE;
  std::ifstream input(referencePath, std::ios::binary);
  if (!input) return EXIT_FAILURE;
  const auto reference = std::make_shared<const std::string>(
      std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  const auto published = publishedAt(track, 0);
  std::cout << "subtitle_presentation_tests: track=" << track->label
            << " cues=" << track->cues.size()
            << " imported-events=" << assEventCount(*track->assScript)
            << " original-events=" << assEventCount(*reference) << '\n';
  int mismatches = 0;
  std::vector<uint8_t> expectedCanvas, actualCanvas;
  for (const int64_t time : {0LL, 59'000'000LL, 60'000'000LL, 75'000'000LL,
                             90'000'000LL, 105'000'000LL, 120'000'000LL,
                             1'350'000'000LL, 1'365'000'000LL, 1'380'000'000LL,
                             1'395'000'000LL, 1'410'000'000LL, 1'420'000'000LL}) {
    const auto expected = renderAssSubtitlesToBgraCanvas(
        reference, track->assFonts, time, 640, 360, &expectedCanvas);
    const auto ui = playback_session::projectWindowUiState(published, at(time));
    const auto actual = renderAssSubtitlesToBgraCanvas(
        ui.subtitleAssScript, ui.subtitleAssFonts, ui.subtitleClockUs,
        640, 360, &actualCanvas);
    const bool match = expected.status != SubtitleAssRenderStatus::Error &&
        actual.status == expected.status && actualCanvas == expectedCanvas;
    std::cout << "  time=" << time / 1'000'000.0
              << " original-visible=" << (expected.status == SubtitleAssRenderStatus::WithGlyph)
              << " imported-visible=" << (actual.status == SubtitleAssRenderStatus::WithGlyph)
              << " match=" << match << '\n';
    if (!match) ++mismatches;
  }
  return mismatches == 0 && assEventCount(*track->assScript) == assEventCount(*reference)
      ? EXIT_SUCCESS : EXIT_FAILURE;
}

int inspectMedia(const std::filesystem::path &path) {
  SubtitleManager manager;
  manager.loadForVideo(path);
  int checked = 0;
  int mismatches = 0;
  std::vector<uint8_t> canvas;
  do {
    const auto track = manager.activeTrackSnapshot();
    if (!track || track->signsOrSongs || track->forced || !track->assScript) continue;
    const auto published = publishedAt(track, 0);
    // Dialogue events lack the positioning/drawing effects used for signs
    // and karaoke. Validate every sentence at its beginning, middle and end.
    for (const auto &cue : track->cues) {
      if (cue.hasPosition || cue.hasMove || cue.hasClip ||
          !cue.transforms.empty() || cue.text.empty()) continue;
      const int64_t margin = std::min<int64_t>(100'000, (cue.endUs - cue.startUs) / 4);
      for (const int64_t time : {cue.startUs + margin,
                                (cue.startUs + cue.endUs) / 2, cue.endUs - margin}) {
        const auto ui = playback_session::projectWindowUiState(published, at(time));
        const auto rendered = renderAssSubtitlesToBgraCanvas(
            ui.subtitleAssScript, ui.subtitleAssFonts, ui.subtitleClockUs,
            640, 360, &canvas);
        if (ui.subtitleClockUs != time || ui.subtitleCues.empty() ||
            rendered.status != SubtitleAssRenderStatus::WithGlyph) ++mismatches;
        ++checked;
      }
    }
  } while (manager.cycleLanguage());
  std::cout << "subtitle_presentation_tests: media samples=" << checked
            << " mismatches=" << mismatches << '\n';
  return checked > 0 && mismatches == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3) return compareAssMedia(pathFromUtf8String(argv[1]), pathFromUtf8String(argv[2]));
  if (argc == 2) return inspectMedia(pathFromUtf8String(argv[1]));
  if (argc != 1) return 2;
  bool ok = exercisePublishedRevision(false);
  ok &= exercisePublishedRevision(true);
  ok &= exerciseSnapshotLifetime();
  ok &= exerciseAssGraphics();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
