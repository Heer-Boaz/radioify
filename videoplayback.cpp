#include "videoplayback.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <sstream>
#include <string_view>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "audioplayback.h"
#include "gpu_shared.h"
#include "player.h"
#include "playback_dialog.h"
#include "ui_helpers.h"
#include "videowindow.h"
#include "playback_mode.h"
#include "playback_frame_output.h"

#include "timing_log.h"

static VideoWindow g_videoWindow;
// Centralized GPU frame cache shared between renderers
static GpuVideoFrameCache g_frameCache;
static bool g_windowEnabledPersistent = false;
static bool g_windowEnabledInitialized = false;

#if RADIOIFY_ENABLE_TIMING_LOG
static inline std::string now_ms() {
  using namespace std::chrono;
  auto t = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
  std::ostringstream ss; ss << t; return ss.str();
}
static inline std::string thread_id_str() {
  std::ostringstream ss; ss << std::this_thread::get_id(); return ss.str();
}
#endif

namespace {
#if RADIOIFY_ENABLE_FFMPEG_ERROR_LOG
std::mutex gFfmpegLogMutex;
std::filesystem::path gFfmpegLogPath;

void ffmpegLogCallback(void* ptr, int level, const char* fmt, va_list vl) {
  if (level > AV_LOG_ERROR) return;
  char line[1024];
  int printPrefix = 1;
  av_log_format_line2(ptr, level, fmt, vl, line, sizeof(line), &printPrefix);
  
  std::lock_guard<std::mutex> lock(timingLogMutex());
  static std::ofstream gFfmpegLogFile;
  static std::filesystem::path gLastPath;

  std::filesystem::path currentPath;
  {
    std::lock_guard<std::mutex> lock2(gFfmpegLogMutex);
    currentPath = gFfmpegLogPath;
  }
  
  if (currentPath.empty()) return;
  if (currentPath != gLastPath) {
    if (gFfmpegLogFile.is_open()) gFfmpegLogFile.close();
    gFfmpegLogFile.open(currentPath, std::ios::app);
    gLastPath = currentPath;
  }
  
  if (!gFfmpegLogFile.is_open()) return;

  // Simple deduplication to prevent flooding during seeking storms
  static std::string lastLine;
  static int repeatCount = 0;
  static auto lastLogTime = std::chrono::steady_clock::now();
  auto now = std::chrono::steady_clock::now();

  std::string currentLine(line);
  if (currentLine == lastLine && (now - lastLogTime < std::chrono::seconds(1))) {
    repeatCount++;
    if (repeatCount == 100) {
       gFfmpegLogFile << radioifyLogTimestamp() << " ffmpeg_error ... multiple repeats suppressed ...\n";
    }
    return;
  }

  if (repeatCount > 0 && repeatCount != 100) {
     gFfmpegLogFile << radioifyLogTimestamp() << " ffmpeg_error (repeated " << repeatCount << " times)\n";
  }

  lastLine = currentLine;
  repeatCount = 0;
  lastLogTime = now;

  gFfmpegLogFile << radioifyLogTimestamp() << " ffmpeg_error " << line;
  size_t len = std::strlen(line);
  if (len == 0 || line[len - 1] != '\n') {
    gFfmpegLogFile << "\n";
  }
}
#endif

const char* playerStateLabel(PlayerState state) {
  switch (state) {
    case PlayerState::Idle:
      return "Idle";
    case PlayerState::Opening:
      return "Opening";
    case PlayerState::Prefill:
      return "Prefill";
    case PlayerState::Priming:
      return "Priming";
    case PlayerState::Playing:
      return "Playing";
    case PlayerState::Paused:
      return "Paused";
    case PlayerState::Seeking:
      return "Seeking";
    case PlayerState::Draining:
      return "Draining";
    case PlayerState::Ended:
      return "Ended";
    case PlayerState::Error:
      return "Error";
    case PlayerState::Closing:
      return "Closing";
  }
  return "Unknown";
}

const char* clockSourceLabel(PlayerClockSource source) {
  switch (source) {
    case PlayerClockSource::None:
      return "none";
    case PlayerClockSource::Audio:
      return "audio";
    case PlayerClockSource::Video:
      return "video";
  }
  return "none";
}

struct SubtitleCue {
  int64_t startUs = 0;
  int64_t endUs = 0;
  std::string text;
};

std::string toLowerAscii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(
        std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

bool startsWith(const std::string& value, const std::string& prefix) {
  if (prefix.size() > value.size()) return false;
  return std::equal(prefix.begin(), prefix.end(), value.begin());
}

std::string_view trimAsciiView(std::string_view v) {
  size_t begin = 0;
  while (begin < v.size() &&
         std::isspace(static_cast<unsigned char>(v[begin])) != 0) {
    ++begin;
  }
  size_t end = v.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(v[end - 1])) != 0) {
    --end;
  }
  return v.substr(begin, end - begin);
}

std::string trimAscii(std::string_view v) {
  std::string_view t = trimAsciiView(v);
  return std::string(t.begin(), t.end());
}

bool isDigitsOnly(std::string_view v) {
  if (v.empty()) return false;
  for (char ch : v) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

bool parseNonNegativeInt(std::string_view v, int* out) {
  if (!out || v.empty()) return false;
  int value = 0;
  for (char ch : v) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return false;
    int digit = ch - '0';
    if (value > ((std::numeric_limits<int>::max)() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

void replaceAll(std::string* text, const std::string& from,
                const std::string& to) {
  if (!text || from.empty()) return;
  size_t pos = 0;
  while ((pos = text->find(from, pos)) != std::string::npos) {
    text->replace(pos, from.size(), to);
    pos += to.size();
  }
}

std::string stripSubtitleMarkup(const std::string& in) {
  std::string noHtml;
  noHtml.reserve(in.size());
  bool inTag = false;
  for (char ch : in) {
    if (ch == '<') {
      inTag = true;
      continue;
    }
    if (inTag) {
      if (ch == '>') inTag = false;
      continue;
    }
    noHtml.push_back(ch);
  }

  std::string noAss;
  noAss.reserve(noHtml.size());
  bool inBrace = false;
  for (char ch : noHtml) {
    if (ch == '{') {
      inBrace = true;
      continue;
    }
    if (inBrace) {
      if (ch == '}') inBrace = false;
      continue;
    }
    noAss.push_back(ch);
  }

  replaceAll(&noAss, "&nbsp;", " ");
  replaceAll(&noAss, "&amp;", "&");
  replaceAll(&noAss, "&lt;", "<");
  replaceAll(&noAss, "&gt;", ">");
  return trimAscii(noAss);
}

std::vector<std::string> splitLinesNormalized(const std::string& raw) {
  std::vector<std::string> lines;
  std::string text = raw;
  if (text.size() >= 3 &&
      static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }

  std::string normalized;
  normalized.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    char ch = text[i];
    if (ch == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        continue;
      }
      normalized.push_back('\n');
      continue;
    }
    normalized.push_back(ch);
  }

  size_t start = 0;
  while (start <= normalized.size()) {
    size_t end = normalized.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(normalized.substr(start));
      break;
    }
    lines.push_back(normalized.substr(start, end - start));
    start = end + 1;
    if (start == normalized.size()) {
      lines.emplace_back();
      break;
    }
  }
  return lines;
}

void appendUtf8Codepoint(uint32_t cp, std::string* out) {
  if (!out) return;
  if (cp <= 0x7Fu) {
    out->push_back(static_cast<char>(cp));
    return;
  }
  if (cp <= 0x7FFu) {
    out->push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return;
  }
  if (cp <= 0xFFFFu) {
    out->push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return;
  }
  if (cp <= 0x10FFFFu) {
    out->push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

bool decodeUtf16ToUtf8(const std::string& rawBytes,
                       bool littleEndian,
                       size_t startOffset,
                       std::string* outUtf8) {
  if (!outUtf8) return false;
  outUtf8->clear();
  if (startOffset > rawBytes.size()) return false;
  if (((rawBytes.size() - startOffset) & 1u) != 0u) return false;

  auto readUnit = [&](size_t i) -> uint16_t {
    uint16_t b0 = static_cast<unsigned char>(rawBytes[i]);
    uint16_t b1 = static_cast<unsigned char>(rawBytes[i + 1]);
    return littleEndian ? static_cast<uint16_t>(b0 | (b1 << 8))
                        : static_cast<uint16_t>(b1 | (b0 << 8));
  };

  size_t i = startOffset;
  while (i + 1 < rawBytes.size()) {
    uint16_t unit = readUnit(i);
    i += 2;
    if (unit == 0xFEFFu) continue;

    uint32_t codepoint = unit;
    if (unit >= 0xD800u && unit <= 0xDBFFu) {
      if (i + 1 >= rawBytes.size()) return false;
      uint16_t low = readUnit(i);
      if (low < 0xDC00u || low > 0xDFFFu) return false;
      i += 2;
      codepoint =
          0x10000u + ((static_cast<uint32_t>(unit) - 0xD800u) << 10) +
          (static_cast<uint32_t>(low) - 0xDC00u);
    } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
      return false;
    }
    appendUtf8Codepoint(codepoint, outUtf8);
  }
  return !outUtf8->empty();
}

bool looksLikeUtf16WithoutBom(const std::string& rawBytes, bool* outLittleEndian) {
  if (!outLittleEndian) return false;
  if (rawBytes.size() < 8) return false;
  const size_t sampleSize = std::min<size_t>(rawBytes.size(), 4096u);
  size_t evenCount = 0;
  size_t oddCount = 0;
  size_t evenZero = 0;
  size_t oddZero = 0;
  for (size_t i = 0; i < sampleSize; ++i) {
    if ((i & 1u) == 0u) {
      ++evenCount;
      if (rawBytes[i] == '\0') ++evenZero;
    } else {
      ++oddCount;
      if (rawBytes[i] == '\0') ++oddZero;
    }
  }
  if (evenCount == 0 || oddCount == 0) return false;

  const double evenRatio = static_cast<double>(evenZero) / evenCount;
  const double oddRatio = static_cast<double>(oddZero) / oddCount;
  if (oddRatio > 0.35 && evenRatio < 0.05) {
    *outLittleEndian = true;
    return true;
  }
  if (evenRatio > 0.35 && oddRatio < 0.05) {
    *outLittleEndian = false;
    return true;
  }
  return false;
}

std::string decodeSubtitleText(const std::string& rawBytes) {
  if (rawBytes.empty()) return {};
  std::string decoded;

  if (rawBytes.size() >= 2) {
    unsigned char b0 = static_cast<unsigned char>(rawBytes[0]);
    unsigned char b1 = static_cast<unsigned char>(rawBytes[1]);
    if (b0 == 0xFFu && b1 == 0xFEu) {
      if (decodeUtf16ToUtf8(rawBytes, true, 2, &decoded)) return decoded;
    } else if (b0 == 0xFEu && b1 == 0xFFu) {
      if (decodeUtf16ToUtf8(rawBytes, false, 2, &decoded)) return decoded;
    }
  }

  bool littleEndian = true;
  if (looksLikeUtf16WithoutBom(rawBytes, &littleEndian) &&
      decodeUtf16ToUtf8(rawBytes, littleEndian, 0, &decoded)) {
    return decoded;
  }
  return rawBytes;
}

bool parsePositiveDouble(std::string_view text, double* outValue) {
  if (!outValue) return false;
  std::string trimmed = trimAscii(text);
  if (trimmed.empty()) return false;
  char* end = nullptr;
  double value = std::strtod(trimmed.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(value) || value <= 0.0) {
    return false;
  }
  *outValue = value;
  return true;
}

bool parseTimecodeUs(std::string_view token, int64_t* outUs) {
  if (!outUs) return false;
  token = trimAsciiView(token);
  if (token.empty()) return false;

  std::string normalized(token);
  for (char& ch : normalized) {
    if (ch == ',') ch = '.';
  }

  std::array<std::string_view, 3> parts{};
  int partCount = 0;
  std::string_view view(normalized);
  size_t begin = 0;
  while (begin <= view.size()) {
    size_t sep = view.find(':', begin);
    std::string_view piece =
        (sep == std::string::npos) ? view.substr(begin)
                                   : view.substr(begin, sep - begin);
    if (piece.empty() || partCount >= 3) return false;
    parts[partCount++] = piece;
    if (sep == std::string::npos) break;
    begin = sep + 1;
  }
  if (partCount != 2 && partCount != 3) return false;

  int hours = 0;
  int minutes = 0;
  std::string_view secPart;
  if (partCount == 3) {
    if (!parseNonNegativeInt(parts[0], &hours)) return false;
    if (!parseNonNegativeInt(parts[1], &minutes)) return false;
    secPart = parts[2];
  } else {
    if (!parseNonNegativeInt(parts[0], &minutes)) return false;
    secPart = parts[1];
  }

  size_t dot = secPart.find('.');
  std::string_view secDigits =
      (dot == std::string::npos) ? secPart : secPart.substr(0, dot);
  int seconds = 0;
  if (!parseNonNegativeInt(secDigits, &seconds)) return false;
  int millis = 0;
  if (dot != std::string::npos) {
    std::string_view frac = secPart.substr(dot + 1);
    if (frac.empty()) return false;
    int digits = 0;
    for (char ch : frac) {
      if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return false;
      if (digits < 3) {
        millis = millis * 10 + (ch - '0');
      }
      ++digits;
    }
    while (digits < 3) {
      millis *= 10;
      ++digits;
    }
  }

  if (minutes >= 60 || seconds >= 60) return false;

  *outUs = (static_cast<int64_t>(hours) * 3600LL +
            static_cast<int64_t>(minutes) * 60LL +
            static_cast<int64_t>(seconds)) *
               1000000LL +
           static_cast<int64_t>(millis) * 1000LL;
  return true;
}

bool parseCueWindow(std::string_view line, int64_t* outStartUs,
                    int64_t* outEndUs) {
  if (!outStartUs || !outEndUs) return false;
  size_t arrow = line.find("-->");
  if (arrow == std::string::npos) return false;
  std::string_view lhs = trimAsciiView(line.substr(0, arrow));
  std::string_view rhs = trimAsciiView(line.substr(arrow + 3));
  if (lhs.empty() || rhs.empty()) return false;
  size_t rhsSpace = rhs.find_first_of(" \t");
  if (rhsSpace != std::string::npos) {
    rhs = rhs.substr(0, rhsSpace);
  }
  int64_t startUs = 0;
  int64_t endUs = 0;
  if (!parseTimecodeUs(lhs, &startUs)) return false;
  if (!parseTimecodeUs(rhs, &endUs)) return false;
  if (endUs <= startUs) {
    endUs = startUs + 100000;
  }
  *outStartUs = startUs;
  *outEndUs = endUs;
  return true;
}

std::string joinCueText(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& line : lines) {
    std::string cleaned = stripSubtitleMarkup(line);
    if (cleaned.empty()) continue;
    if (!out.empty()) out.push_back('\n');
    out += cleaned;
  }
  return out;
}

bool parseSrtCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  size_t i = 0;
  while (i < lines.size()) {
    while (i < lines.size() && trimAsciiView(lines[i]).empty()) {
      ++i;
    }
    if (i >= lines.size()) break;

    std::string_view timingLine = trimAsciiView(lines[i]);
    if (timingLine.find("-->") == std::string::npos) {
      if (isDigitsOnly(timingLine) &&
          i + 1 < lines.size()) {
        ++i;
        timingLine = trimAsciiView(lines[i]);
      }
    }
    if (timingLine.find("-->") == std::string::npos) {
      ++i;
      continue;
    }

    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseCueWindow(timingLine, &startUs, &endUs)) {
      ++i;
      continue;
    }
    ++i;

    std::vector<std::string> cueLines;
    while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
      cueLines.push_back(lines[i]);
      ++i;
    }

    std::string text = joinCueText(cueLines);
    if (!text.empty()) {
      outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
    }
  }
  return !outCues->empty();
}

bool parseVttCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  size_t i = 0;
  if (!lines.empty()) {
    std::string header = toLowerAscii(trimAscii(lines[0]));
    if (header.rfind("webvtt", 0) == 0) {
      i = 1;
    }
  }

  while (i < lines.size()) {
    std::string_view line = trimAsciiView(lines[i]);
    if (line.empty()) {
      ++i;
      continue;
    }
    if (line.rfind("NOTE", 0) == 0 || line.rfind("note", 0) == 0) {
      ++i;
      while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
        ++i;
      }
      continue;
    }

    std::string_view timingLine = line;
    if (timingLine.find("-->") == std::string::npos) {
      ++i;
      if (i >= lines.size()) break;
      timingLine = trimAsciiView(lines[i]);
    }
    if (timingLine.find("-->") == std::string::npos) {
      ++i;
      continue;
    }

    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseCueWindow(timingLine, &startUs, &endUs)) {
      ++i;
      continue;
    }
    ++i;

    std::vector<std::string> cueLines;
    while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
      cueLines.push_back(lines[i]);
      ++i;
    }
    std::string text = joinCueText(cueLines);
    if (!text.empty()) {
      outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
    }
  }
  return !outCues->empty();
}

bool parseSbvCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  size_t i = 0;
  while (i < lines.size()) {
    while (i < lines.size() && trimAsciiView(lines[i]).empty()) {
      ++i;
    }
    if (i >= lines.size()) break;

    std::string_view timingLine = trimAsciiView(lines[i]);
    size_t comma = timingLine.find(',');
    if (comma == std::string::npos) {
      ++i;
      continue;
    }
    std::string_view lhs = trimAsciiView(timingLine.substr(0, comma));
    std::string_view rhs = trimAsciiView(timingLine.substr(comma + 1));
    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseTimecodeUs(lhs, &startUs) || !parseTimecodeUs(rhs, &endUs)) {
      ++i;
      continue;
    }
    if (endUs <= startUs) endUs = startUs + 100000;
    ++i;

    std::vector<std::string> cueLines;
    while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
      cueLines.push_back(lines[i]);
      ++i;
    }
    std::string text = joinCueText(cueLines);
    if (!text.empty()) {
      outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
    }
  }

  return !outCues->empty();
}

bool parseMicroDvdLine(std::string_view line, int* outStartFrame, int* outEndFrame,
                       std::string* outText) {
  if (!outStartFrame || !outEndFrame || !outText) return false;
  line = trimAsciiView(line);
  if (line.size() < 7 || line.front() != '{') return false;

  auto parseFrameTag = [&](size_t startPos, int* outFrame, size_t* outNextPos) {
    if (!outFrame || !outNextPos || startPos >= line.size() ||
        line[startPos] != '{') {
      return false;
    }
    size_t close = line.find('}', startPos + 1);
    if (close == std::string::npos || close <= startPos + 1) return false;
    std::string_view digits = line.substr(startPos + 1, close - startPos - 1);
    if (!parseNonNegativeInt(digits, outFrame)) return false;
    *outNextPos = close + 1;
    return true;
  };

  size_t pos = 0;
  int startFrame = 0;
  int endFrame = 0;
  if (!parseFrameTag(pos, &startFrame, &pos)) return false;
  if (!parseFrameTag(pos, &endFrame, &pos)) return false;
  if (startFrame < 0 || endFrame < 0) return false;

  *outStartFrame = startFrame;
  *outEndFrame = endFrame;
  *outText = trimAscii(line.substr(pos));
  return true;
}

bool parseMicroDvdCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  double fps = 25.0;
  bool fpsExplicit = false;
  for (const std::string& rawLine : lines) {
    int startFrame = 0;
    int endFrame = 0;
    std::string text;
    if (!parseMicroDvdLine(rawLine, &startFrame, &endFrame, &text)) {
      continue;
    }

    if ((startFrame == 1 && endFrame == 1) ||
        (startFrame == 0 && endFrame == 0)) {
      double parsedFps = 0.0;
      if (parsePositiveDouble(text, &parsedFps)) {
        fps = parsedFps;
        fpsExplicit = true;
        continue;
      }
      if (!fpsExplicit) continue;
    }

    if (fps <= 0.0 || !std::isfinite(fps)) continue;
    replaceAll(&text, "|", "\n");
    text = stripSubtitleMarkup(text);
    if (text.empty()) continue;

    int64_t startUs = static_cast<int64_t>(
        std::llround((static_cast<double>(startFrame) * 1000000.0) / fps));
    int64_t endUs = static_cast<int64_t>(
        std::llround((static_cast<double>(endFrame) * 1000000.0) / fps));
    if (endUs <= startUs) endUs = startUs + 100000;
    outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
  }

  return !outCues->empty();
}

bool splitCsvPrefix(const std::string& text, size_t fieldCount,
                    std::vector<std::string>* outFields) {
  if (!outFields) return false;
  outFields->clear();
  if (fieldCount == 0) return false;
  outFields->reserve(fieldCount);
  size_t start = 0;
  for (size_t i = 0; i + 1 < fieldCount; ++i) {
    size_t comma = text.find(',', start);
    if (comma == std::string::npos) return false;
    outFields->push_back(trimAscii(text.substr(start, comma - start)));
    start = comma + 1;
  }
  outFields->push_back(trimAscii(text.substr(start)));
  return outFields->size() == fieldCount;
}

bool parseAssCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  bool inEvents = false;
  std::vector<std::string> formatFields;
  int startIndex = -1;
  int endIndex = -1;
  int textIndex = -1;

  auto refreshFormatIndexes = [&]() {
    startIndex = -1;
    endIndex = -1;
    textIndex = -1;
    for (size_t i = 0; i < formatFields.size(); ++i) {
      std::string name = toLowerAscii(trimAscii(formatFields[i]));
      if (name == "start") startIndex = static_cast<int>(i);
      if (name == "end") endIndex = static_cast<int>(i);
      if (name == "text") textIndex = static_cast<int>(i);
    }
  };

  for (const std::string& rawLine : lines) {
    std::string line = trimAscii(rawLine);
    if (line.empty()) continue;

    if (!line.empty() && line.front() == '[' && line.back() == ']') {
      std::string section = toLowerAscii(line);
      inEvents = (section == "[events]");
      continue;
    }
    if (!inEvents) continue;

    const std::string formatPrefix = "Format:";
    const std::string dialoguePrefix = "Dialogue:";
    if (line.size() >= formatPrefix.size() &&
        startsWith(toLowerAscii(line.substr(0, formatPrefix.size())),
                   toLowerAscii(formatPrefix))) {
      std::string payload = line.substr(formatPrefix.size());
      formatFields.clear();
      size_t start = 0;
      while (start <= payload.size()) {
        size_t comma = payload.find(',', start);
        std::string field =
            (comma == std::string::npos)
                ? payload.substr(start)
                : payload.substr(start, comma - start);
        formatFields.push_back(trimAscii(field));
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
      refreshFormatIndexes();
      continue;
    }

    if (line.size() < dialoguePrefix.size() ||
        !startsWith(toLowerAscii(line.substr(0, dialoguePrefix.size())),
                    toLowerAscii(dialoguePrefix))) {
      continue;
    }
    if (formatFields.empty() || startIndex < 0 || endIndex < 0 ||
        textIndex < 0) {
      continue;
    }

    std::string payload = line.substr(dialoguePrefix.size());
    std::vector<std::string> fields;
    if (!splitCsvPrefix(payload, formatFields.size(), &fields)) {
      continue;
    }

    if (startIndex >= static_cast<int>(fields.size()) ||
        endIndex >= static_cast<int>(fields.size()) ||
        textIndex >= static_cast<int>(fields.size())) {
      continue;
    }

    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseTimecodeUs(fields[static_cast<size_t>(startIndex)], &startUs) ||
        !parseTimecodeUs(fields[static_cast<size_t>(endIndex)], &endUs)) {
      continue;
    }
    if (endUs <= startUs) {
      endUs = startUs + 100000;
    }

    std::string text = fields[static_cast<size_t>(textIndex)];
    replaceAll(&text, "\\N", "\n");
    replaceAll(&text, "\\n", "\n");
    text = stripSubtitleMarkup(text);
    if (text.empty()) continue;
    outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
  }
  return !outCues->empty();
}

struct SubtitleTrack {
  std::string label;
  std::vector<SubtitleCue> cues;
  mutable size_t lastCueIndex = 0;

  const SubtitleCue* cueAt(int64_t clockUs) const {
    if (cues.empty()) return nullptr;
    if (lastCueIndex < cues.size()) {
      const SubtitleCue& hintCue = cues[lastCueIndex];
      if (clockUs >= hintCue.startUs && clockUs < hintCue.endUs) {
        return &hintCue;
      }
      if (clockUs >= hintCue.endUs && lastCueIndex + 1 < cues.size()) {
        const SubtitleCue& nextCue = cues[lastCueIndex + 1];
        if (clockUs >= nextCue.startUs && clockUs < nextCue.endUs) {
          ++lastCueIndex;
          return &nextCue;
        }
      }
    }

    auto it = std::upper_bound(
        cues.begin(), cues.end(), clockUs,
        [](int64_t t, const SubtitleCue& cue) { return t < cue.startUs; });
    if (it == cues.begin()) return nullptr;
    --it;
    if (clockUs < it->startUs || clockUs >= it->endUs) return nullptr;
    lastCueIndex = static_cast<size_t>(std::distance(cues.begin(), it));
    return &(*it);
  }

  void resetLookup() const { lastCueIndex = 0; }
};

bool loadSubtitleTrackFile(const std::filesystem::path& path,
                           SubtitleTrack* outTrack) {
  auto tryParser = [](const std::string& payload,
                      bool (*parser)(const std::string&, std::vector<SubtitleCue>*),
                      std::vector<SubtitleCue>* outCues) {
    if (!parser || !outCues) return false;
    std::vector<SubtitleCue> parsed;
    if (!parser(payload, &parsed) || parsed.empty()) return false;
    *outCues = std::move(parsed);
    return true;
  };

  auto parseBySniffing = [&](const std::string& payload,
                             std::vector<SubtitleCue>* outCues) {
    return tryParser(payload, &parseSrtCues, outCues) ||
           tryParser(payload, &parseVttCues, outCues) ||
           tryParser(payload, &parseAssCues, outCues) ||
           tryParser(payload, &parseSbvCues, outCues) ||
           tryParser(payload, &parseMicroDvdCues, outCues);
  };

  if (!outTrack) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::string rawBytes((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  if (rawBytes.empty()) return false;
  std::string raw = decodeSubtitleText(rawBytes);
  if (raw.empty()) return false;

  std::string ext = toLowerAscii(path.extension().string());
  bool ok = false;
  if (ext == ".srt") {
    ok = tryParser(raw, &parseSrtCues, &outTrack->cues);
  } else if (ext == ".vtt") {
    ok = tryParser(raw, &parseVttCues, &outTrack->cues);
  } else if (ext == ".ass" || ext == ".ssa") {
    ok = tryParser(raw, &parseAssCues, &outTrack->cues);
  } else if (ext == ".sbv") {
    ok = tryParser(raw, &parseSbvCues, &outTrack->cues);
  } else if (ext == ".sub") {
    ok = tryParser(raw, &parseMicroDvdCues, &outTrack->cues);
  } else if (ext == ".txt" || ext == ".smi" || ext == ".sami") {
    ok = parseBySniffing(raw, &outTrack->cues);
  } else {
    ok = parseBySniffing(raw, &outTrack->cues);
  }
  if (!ok) {
    ok = parseBySniffing(raw, &outTrack->cues);
  }
  if (!ok || outTrack->cues.empty()) {
    outTrack->cues.clear();
    return false;
  }

  std::sort(outTrack->cues.begin(), outTrack->cues.end(),
            [](const SubtitleCue& a, const SubtitleCue& b) {
              if (a.startUs != b.startUs) return a.startUs < b.startUs;
              return a.endUs < b.endUs;
            });
  outTrack->resetLookup();
  return true;
}

bool isSubtitleSidecarExtension(const std::string& extLower) {
  return extLower == ".srt" || extLower == ".vtt" || extLower == ".ass" ||
         extLower == ".ssa" || extLower == ".sbv" || extLower == ".sub" ||
         extLower == ".txt" || extLower == ".smi" || extLower == ".sami";
}

bool isSubtitleDirectoryName(const std::string& nameLower) {
  return nameLower == "subs" || nameLower == "sub" ||
         nameLower == "subtitle" || nameLower == "subtitles";
}

std::vector<std::filesystem::path> discoverSubtitleFiles(
    const std::filesystem::path& videoPath) {
  std::vector<std::pair<int, std::filesystem::path>> ranked;
  std::vector<std::filesystem::path> fallbackCandidates;
  std::error_code ec;
  std::filesystem::path dir = videoPath.parent_path();
  if (dir.empty()) {
    dir = std::filesystem::current_path(ec);
    if (ec) return {};
  }
  std::string baseStem = videoPath.stem().string();
  std::string baseStemLower = toLowerAscii(baseStem);

  auto rankForStem = [&](const std::string& stem) -> int {
    if (baseStemLower.empty()) return -1;
    const std::string stemLower = toLowerAscii(stem);
    if (stemLower == baseStemLower) return 0;
    if (startsWith(stemLower, baseStemLower + ".")) return 1;
    if (startsWith(stemLower, baseStemLower + "_")) return 1;
    if (startsWith(stemLower, baseStemLower + "-")) return 1;
    if (startsWith(stemLower, baseStemLower + " ")) return 1;
    if (startsWith(stemLower, baseStemLower + "(")) return 1;
    if (stemLower.find(baseStemLower) != std::string::npos) return 3;
    return -1;
  };

  struct SearchDir {
    std::filesystem::path path;
    int maxDepth = 0;
  };
  std::vector<SearchDir> searchDirs;
  auto addSearchDir = [&](const std::filesystem::path& searchDir, int maxDepth) {
    std::string key = toLowerAscii(searchDir.lexically_normal().string());
    for (const auto& existing : searchDirs) {
      if (toLowerAscii(existing.path.lexically_normal().string()) == key) {
        return;
      }
    }
    searchDirs.push_back(SearchDir{searchDir, std::max(0, maxDepth)});
  };

  addSearchDir(dir, 0);
  for (std::filesystem::directory_iterator it(dir, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    const auto& entry = *it;
    bool isDir = entry.is_directory(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!isDir) continue;
    std::string nameLower =
        toLowerAscii(entry.path().filename().string());
    if (isSubtitleDirectoryName(nameLower)) {
      addSearchDir(entry.path(), 2);
    }
  }
  if (ec) ec.clear();

  auto maybeAddCandidate = [&](const std::filesystem::path& candidate) {
    std::string extLower = toLowerAscii(candidate.extension().string());
    if (!isSubtitleSidecarExtension(extLower)) return;
    int rank = rankForStem(candidate.stem().string());
    if (rank >= 0) {
      ranked.emplace_back(rank, candidate);
    } else {
      fallbackCandidates.push_back(candidate);
    }
  };

  auto scanFlatDirectory = [&](const std::filesystem::path& searchDir) {
    for (std::filesystem::directory_iterator it(searchDir, ec), end; it != end;
         it.increment(ec)) {
      if (ec) break;
      const auto& entry = *it;
      bool isRegular = entry.is_regular_file(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (!isRegular) continue;
      maybeAddCandidate(entry.path());
    }
    if (ec) ec.clear();
  };

  auto scanRecursiveDirectory = [&](const std::filesystem::path& searchDir,
                                    int maxDepth) {
    std::filesystem::directory_options options =
        std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(searchDir, options, ec),
         end;
         it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      const auto& entry = *it;
      bool isDir = entry.is_directory(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (isDir) {
        if (it.depth() >= maxDepth) {
          it.disable_recursion_pending();
        }
        continue;
      }
      bool isRegular = entry.is_regular_file(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (!isRegular) continue;
      maybeAddCandidate(entry.path());
    }
    if (ec) ec.clear();
  };

  for (const auto& searchDir : searchDirs) {
    bool dirExists = std::filesystem::exists(searchDir.path, ec);
    if (ec || !dirExists) {
      ec.clear();
      continue;
    }
    bool isDir = std::filesystem::is_directory(searchDir.path, ec);
    if (ec || !isDir) {
      ec.clear();
      continue;
    }
    if (searchDir.maxDepth <= 0) {
      scanFlatDirectory(searchDir.path);
    } else {
      scanRecursiveDirectory(searchDir.path, searchDir.maxDepth);
    }
  }

  if (ranked.empty()) {
    for (const auto& candidate : fallbackCandidates) {
      ranked.emplace_back(100, candidate);
    }
  }

  std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) {
              if (a.first != b.first) return a.first < b.first;
              return toLowerAscii(a.second.string()) <
                     toLowerAscii(b.second.string());
            });

  ranked.erase(std::unique(ranked.begin(), ranked.end(),
                           [](const auto& a, const auto& b) {
                             return toLowerAscii(a.second.string()) ==
                                    toLowerAscii(b.second.string());
                           }),
               ranked.end());

  std::vector<std::filesystem::path> out;
  out.reserve(ranked.size());
  for (const auto& item : ranked) {
    out.push_back(item.second);
  }
  return out;
}

std::string subtitleLabelFromPath(const std::filesystem::path& path,
                                  const std::string& baseStem) {
  std::string stem = path.stem().string();
  std::string label;
  std::string prefix = baseStem + ".";
  if (stem.rfind(prefix, 0) == 0 && stem.size() > prefix.size()) {
    label = stem.substr(prefix.size());
  } else {
    label = path.extension().string();
    if (!label.empty() && label[0] == '.') {
      label.erase(0, 1);
    }
  }
  if (label.empty()) label = "Track";
  for (char& ch : label) {
    if (ch == '_' || ch == '-') ch = ' ';
  }
  return label;
}

std::string subtitleStreamMetadata(const AVStream* stream, const char* key) {
  if (!stream || !stream->metadata || !key) return {};
  const AVDictionaryEntry* entry =
      av_dict_get(stream->metadata, key, nullptr, 0);
  if (!entry || !entry->value || entry->value[0] == '\0') return {};
  return std::string(entry->value);
}

std::string subtitleLabelFromStream(const AVStream* stream, size_t ordinal) {
  std::string language = subtitleStreamMetadata(stream, "language");
  std::string title = subtitleStreamMetadata(stream, "title");
  if (!title.empty() && !language.empty()) {
    return language + " - " + title;
  }
  if (!language.empty()) return language;
  if (!title.empty()) return title;
  return "Embedded " + std::to_string(ordinal + 1);
}

bool isTextSubtitleCodec(AVCodecID codecId) {
  switch (codecId) {
    case AV_CODEC_ID_TEXT:
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_WEBVTT:
    case AV_CODEC_ID_MOV_TEXT:
#ifdef AV_CODEC_ID_MICRODVD
    case AV_CODEC_ID_MICRODVD:
#endif
#ifdef AV_CODEC_ID_JACOSUB
    case AV_CODEC_ID_JACOSUB:
#endif
#ifdef AV_CODEC_ID_SUBVIEWER
    case AV_CODEC_ID_SUBVIEWER:
#endif
#ifdef AV_CODEC_ID_SUBVIEWER1
    case AV_CODEC_ID_SUBVIEWER1:
#endif
      return true;
    default:
      return false;
  }
}

int64_t packetPtsUs(const AVPacket& pkt, AVRational timeBase) {
  int64_t pts = pkt.pts;
  if (pts == AV_NOPTS_VALUE) pts = pkt.dts;
  if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
  return av_rescale_q(pts, timeBase, AVRational{1, 1000000});
}

int64_t packetDurationUs(const AVPacket& pkt, AVRational timeBase) {
  if (pkt.duration <= 0) return 0;
  int64_t durUs = av_rescale_q(pkt.duration, timeBase, AVRational{1, 1000000});
  return durUs > 0 ? durUs : 0;
}

std::string assPayloadToText(const std::string& payload) {
  std::string text = payload;
  std::string lower = toLowerAscii(trimAscii(payload));
  if (startsWith(lower, "dialogue:")) {
    size_t colon = payload.find(':');
    if (colon != std::string::npos) {
      text = payload.substr(colon + 1);
    }
  }

  size_t start = 0;
  int commas = 0;
  while (start < text.size() && commas < 8) {
    size_t comma = text.find(',', start);
    if (comma == std::string::npos) break;
    start = comma + 1;
    ++commas;
  }
  if (commas == 8 && start < text.size()) {
    text = text.substr(start);
  }
  replaceAll(&text, "\\N", "\n");
  replaceAll(&text, "\\n", "\n");
  return stripSubtitleMarkup(text);
}

std::string packetSubtitleText(AVCodecID codecId, const AVPacket& pkt) {
  if (!pkt.data || pkt.size <= 0) return {};
  std::string payload(reinterpret_cast<const char*>(pkt.data),
                      reinterpret_cast<const char*>(pkt.data + pkt.size));
  if (codecId == AV_CODEC_ID_ASS || codecId == AV_CODEC_ID_SSA) {
    return assPayloadToText(payload);
  }
  replaceAll(&payload, "\\N", "\n");
  replaceAll(&payload, "\\n", "\n");
  return stripSubtitleMarkup(payload);
}

bool loadEmbeddedSubtitleTracks(const std::filesystem::path& videoPath,
                                std::vector<SubtitleTrack>* outTracks) {
  if (!outTracks) return false;
  outTracks->clear();

  AVFormatContext* fmt = nullptr;
  std::string pathUtf8 = toUtf8String(videoPath);
  if (avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, nullptr) < 0) {
    return false;
  }
  struct Scope {
    AVFormatContext** fmt = nullptr;
    ~Scope() {
      if (fmt && *fmt) avformat_close_input(fmt);
    }
  } scope{&fmt};

  (void)avformat_find_stream_info(fmt, nullptr);

  struct EmbeddedTrackInfo {
    int streamIndex = -1;
    AVRational timeBase{0, 1};
    AVCodecID codecId = AV_CODEC_ID_NONE;
    SubtitleTrack track;
  };
  std::vector<EmbeddedTrackInfo> refs;

  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    AVStream* stream = fmt->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
    EmbeddedTrackInfo info;
    info.streamIndex = static_cast<int>(i);
    info.timeBase = stream->time_base;
    info.codecId = stream->codecpar->codec_id;
    info.track.label = subtitleLabelFromStream(stream, refs.size());
    refs.push_back(std::move(info));
  }

  if (refs.empty()) return false;
  int64_t formatStartUs =
      (fmt->start_time != AV_NOPTS_VALUE) ? fmt->start_time : 0;

  AVPacket* pkt = av_packet_alloc();
  if (!pkt) {
    for (auto& ref : refs) {
      ref.track.resetLookup();
      outTracks->push_back(std::move(ref.track));
    }
    return !outTracks->empty();
  }

  while (av_read_frame(fmt, pkt) >= 0) {
    for (auto& ref : refs) {
      if (pkt->stream_index != ref.streamIndex) continue;
      if (!isTextSubtitleCodec(ref.codecId)) break;
      std::string text = packetSubtitleText(ref.codecId, *pkt);
      if (text.empty()) break;
      int64_t startUs = packetPtsUs(*pkt, ref.timeBase);
      if (startUs == AV_NOPTS_VALUE) break;
      startUs -= formatStartUs;
      if (startUs < 0) startUs = 0;
      int64_t endUs = startUs + packetDurationUs(*pkt, ref.timeBase);
      if (endUs <= startUs) {
        endUs = startUs + 2000000;  // 2 seconds fallback
      }
      ref.track.cues.push_back(SubtitleCue{startUs, endUs, std::move(text)});
      break;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);

  for (auto& ref : refs) {
    if (!ref.track.cues.empty()) {
      std::sort(ref.track.cues.begin(), ref.track.cues.end(),
                [](const SubtitleCue& a, const SubtitleCue& b) {
                  if (a.startUs != b.startUs) return a.startUs < b.startUs;
                  return a.endUs < b.endUs;
                });
    }
    ref.track.resetLookup();
    outTracks->push_back(std::move(ref.track));
  }
  return !outTracks->empty();
}

class SubtitleManager {
 public:
  void loadForVideo(const std::filesystem::path& videoPath) {
    tracks_.clear();
    activeTrack_ = 0;

    const std::string baseStem = videoPath.stem().string();
    std::vector<std::filesystem::path> files = discoverSubtitleFiles(videoPath);
    for (const auto& subtitleFile : files) {
      SubtitleTrack track;
      track.label = subtitleLabelFromPath(subtitleFile, baseStem);
      if (!loadSubtitleTrackFile(subtitleFile, &track)) {
        continue;
      }
      track.label = makeUniqueLabel(track.label);
      tracks_.push_back(std::move(track));
    }

    std::vector<SubtitleTrack> embeddedTracks;
    if (loadEmbeddedSubtitleTracks(videoPath, &embeddedTracks)) {
      for (auto& embedded : embeddedTracks) {
        embedded.label = makeUniqueLabel(embedded.label);
        tracks_.push_back(std::move(embedded));
      }
    }

    activeTrack_ = 0;
    for (size_t i = 0; i < tracks_.size(); ++i) {
      if (!tracks_[i].cues.empty()) {
        activeTrack_ = i;
        break;
      }
    }
  }

  size_t trackCount() const { return tracks_.size(); }

  size_t activeTrackIndex() const {
    if (tracks_.empty() || activeTrack_ >= tracks_.size()) {
      return static_cast<size_t>(-1);
    }
    return activeTrack_;
  }

  const SubtitleTrack* activeTrack() const {
    if (tracks_.empty() || activeTrack_ >= tracks_.size()) return nullptr;
    return &tracks_[activeTrack_];
  }

  std::string activeTrackLabel() const {
    if (tracks_.empty() || activeTrack_ >= tracks_.size()) return "N/A";
    return tracks_[activeTrack_].label.empty() ? "N/A"
                                               : tracks_[activeTrack_].label;
  }

  bool cycleLanguage() {
    if (tracks_.empty()) return false;
    activeTrack_ = (activeTrack_ + 1) % tracks_.size();
    tracks_[activeTrack_].resetLookup();
    return true;
  }

 private:
  std::string makeUniqueLabel(const std::string& rawLabel) const {
    std::string base = rawLabel.empty() ? "Track" : rawLabel;
    std::string candidate = base;
    int suffix = 2;
    auto exists = [&](const std::string& label) {
      return std::any_of(
          tracks_.begin(), tracks_.end(),
          [&](const SubtitleTrack& t) { return t.label == label; });
    };
    while (exists(candidate)) {
      candidate = base + " " + std::to_string(suffix++);
    }
    return candidate;
  }

  std::vector<SubtitleTrack> tracks_;
  size_t activeTrack_ = 0;
};

struct PerfLog {
  std::ofstream file;
  std::string buffer;
  bool enabled = false;
};

bool perfLogOpen(PerfLog* log, const std::filesystem::path& path,
                 std::string* error) {
#if RADIOIFY_ENABLE_TIMING_LOG
  if (!log) return false;
  log->file.open(path, std::ios::out | std::ios::app);
  if (!log->file.is_open()) {
    if (error) {
      *error = "Failed to open timing log file: " + toUtf8String(path);
    }
    return false;
  }
  log->enabled = true;
  return true;
#else
  (void)path;
  (void)error;
  if (log) log->enabled = false;
  return true;
#endif
}

void perfLogFlush(PerfLog* log) {
  if (!log || !log->enabled || log->buffer.empty()) return;
  log->file << log->buffer;
  log->file.flush();
  log->buffer.clear();
}

void perfLogAppendf(PerfLog* log, const char* fmt, ...) {
  if (!log || !log->enabled) return;
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (written <= 0) return;
  if (written >= static_cast<int>(sizeof(buf))) {
    written = static_cast<int>(sizeof(buf)) - 1;
  }
  std::string ts = radioifyLogTimestamp();
  log->buffer.append(ts);
  log->buffer.push_back(' ');
  log->buffer.append(buf, buf + written);
  log->buffer.push_back('\n');
  if (log->buffer.size() >= 8192) perfLogFlush(log);
}

void perfLogClose(PerfLog* log) {
  if (!log) return;
  perfLogFlush(log);
  if (log->file.is_open()) {
    log->file.close();
  }
  log->enabled = false;
}

void finalizeVideoPlayback(ConsoleScreen& screen, bool fullRedrawEnabled,
                           PerfLog* log) {
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(false);
  }
  perfLogClose(log);
}
}  // namespace

void configureFfmpegVideoLog(const std::filesystem::path& path) {
#if RADIOIFY_ENABLE_FFMPEG_ERROR_LOG
  {
    std::lock_guard<std::mutex> lock(gFfmpegLogMutex);
    gFfmpegLogPath = path;
  }
  av_log_set_level(AV_LOG_ERROR);
  av_log_set_callback(ffmpegLogCallback);
#else
  (void)path;
  av_log_set_level(AV_LOG_QUIET);
#endif
}

bool showAsciiVideo(const std::filesystem::path& file, ConsoleInput& input,
                    ConsoleScreen& screen, const Style& baseStyle,
                    const Style& accentStyle, const Style& dimStyle,
                    const Style& progressEmptyStyle,
                    const Style& progressFrameStyle,
                    const Color& progressStart, const Color& progressEnd,
                    const VideoPlaybackConfig& config,
                    bool* quitAppRequested) {
  bool quitApplicationRequested = false;
  if (quitAppRequested) {
    *quitAppRequested = false;
  }
  struct QuitFlagScope {
    bool* out = nullptr;
    bool* value = nullptr;
    ~QuitFlagScope() {
      if (out && value) {
        *out = *value;
      }
    }
  } quitFlagScope{quitAppRequested, &quitApplicationRequested};

  bool enableAscii = config.enableAscii;
  bool enableAudio = config.enableAudio && audioIsEnabled();
  // Ensure previous video textures/fences are not carried into a new session.
  g_frameCache.Reset();

  bool fullRedrawEnabled = enableAscii;
  if (fullRedrawEnabled) {
    screen.setAlwaysFullRedraw(true);
  }

  auto showError = [&](const std::string& message,
                       const std::string& detail) -> bool {
    playback_dialog::showInfoDialog(input, screen, baseStyle, accentStyle,
                                   dimStyle, "Video error", message, detail,
                                   "");
    return true;
  };

  auto showAudioFallbackPrompt = [&](const std::string& message,
                                     const std::string& detail) -> bool {
    return playback_dialog::showConfirmDialog(
               input, screen, baseStyle, accentStyle, dimStyle, "Audio only?",
               message, detail, "") == playback_dialog::DialogResult::Confirmed;
  };

  PerfLog perfLog;
  std::string logError;
  std::filesystem::path logPath =
      std::filesystem::current_path() / "radioify.log";
  configureFfmpegVideoLog(logPath);
  if (!perfLogOpen(&perfLog, logPath, &logError)) {
    bool ok = showError("Failed to open timing log file.", logError);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }
  perfLogAppendf(&perfLog, "video_start file=%s",
                 toUtf8String(file.filename()).c_str());

  auto appendTiming = [&](const std::string& s) {
#if RADIOIFY_ENABLE_TIMING_LOG
    std::lock_guard<std::mutex> lock(timingLogMutex());
    if (perfLog.file.is_open()) {
      perfLog.file << radioifyLogTimestamp() << " " << s << "\n";
    }
#else
    (void)s;
#endif
  };
  auto appendTimingFmt = [&](const char* fmt, ...) {
#if RADIOIFY_ENABLE_TIMING_LOG
    if (!fmt) return;
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    int written = std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (written <= 0) return;
    if (written >= static_cast<int>(sizeof(buf))) {
      written = static_cast<int>(sizeof(buf)) - 1;
    }
    std::lock_guard<std::mutex> lock(timingLogMutex());
    if (perfLog.file.is_open()) {
      perfLog.file << radioifyLogTimestamp() << " " << std::string(buf, buf + written)
                   << "\n";
    }
#else
    (void)fmt;
#endif
  };

  auto appendVideoError = [&](const std::string& message,
                              const std::string& detail) {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
    std::string line = message.empty() ? "Video error." : message;
    std::string extra = detail;
    if (line.empty() && !extra.empty()) {
      line = extra;
      extra.clear();
    }
    if (line.empty()) {
      line = "Video playback error.";
    }
    std::string payload = "video_error msg=" + line;
    if (!extra.empty()) {
      payload += " detail=" + extra;
    }
    std::ofstream f(logPath, std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << payload << "\n";
#else
    (void)message;
    (void)detail;
#endif
  };

  auto appendVideoWarning = [&](const std::string& message) {
#if RADIOIFY_ENABLE_VIDEO_ERROR_LOG
    std::string line = message.empty() ? "Video warning." : message;
    std::string payload = "video_warning msg=" + line;
    std::ofstream f(logPath, std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << payload << "\n";
#else
    (void)message;
#endif
  };

  auto reportVideoError = [&](const std::string& message,
                              const std::string& detail) -> bool {
    appendVideoError(message, detail);
    std::string uiMessage = message.empty() ? "Video playback error." : message;
    std::string uiDetail = detail;
    if (uiMessage.empty() && uiDetail.empty()) {
      uiDetail = "Video playback encountered an unexpected error.";
    }
    return showError(uiMessage, uiDetail);
  };

  Player player;
  PlayerConfig playerConfig;
  playerConfig.file = file;
  playerConfig.logPath = logPath;
  playerConfig.enableAudio = enableAudio;
  playerConfig.allowDecoderScale = enableAscii;

  if (!player.open(playerConfig, nullptr)) {
    bool ok = reportVideoError("Failed to open video.", "");
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  constexpr auto kPrepRedrawInterval = std::chrono::milliseconds(120);
  constexpr double kPrepPulseSeconds = 1.6;
  auto renderPreparingScreen = [&](double progress) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    screen.clear(baseStyle);
    std::string title = "Video: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    std::string message = "Preparing video playback...";
    int msgLine = std::clamp(height / 2, 1, std::max(1, height - 2));
    int msgWidth = utf8CodepointCount(message);
    if (msgWidth >= width) {
      screen.writeText(0, msgLine, fitLine(message, width), dimStyle);
    } else {
      int msgX = (width - msgWidth) / 2;
      screen.writeText(msgX, msgLine, message, dimStyle);
    }
    int barWidth = std::min(32, width - 6);
    int barLine = msgLine + 1;
    if (barWidth >= 5 && barLine < height) {
      int barX = std::max(0, (width - (barWidth + 2)) / 2);
      screen.writeChar(barX, barLine, L'|', progressFrameStyle);
      auto barCells = renderProgressBarCells(progress, barWidth,
                                             progressEmptyStyle, progressStart,
                                             progressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(barX + 1 + i, barLine, cell.ch, cell.style);
      }
      screen.writeChar(barX + 1 + barWidth, barLine, L'|',
                       progressFrameStyle);
    }
    screen.draw();
  };

  bool running = true;
  auto initStart = std::chrono::steady_clock::now();
  auto lastInitDraw = std::chrono::steady_clock::time_point::min();
  while (running) {
    if (player.initDone()) {
      break;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - lastInitDraw >= kPrepRedrawInterval) {
      double elapsed = std::chrono::duration<double>(now - initStart).count();
      double phase = std::fmod(elapsed, kPrepPulseSeconds);
      double ratio = (phase <= (kPrepPulseSeconds * 0.5))
                         ? (phase / (kPrepPulseSeconds * 0.5))
                         : ((kPrepPulseSeconds - phase) /
                            (kPrepPulseSeconds * 0.5));
      renderPreparingScreen(ratio);
      lastInitDraw = now;
    }
    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if (ctrl && (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q')) {
          quitApplicationRequested = true;
          running = false;
          break;
        }
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == VK_ESCAPE || key.vk == VK_BROWSER_BACK ||
            key.vk == VK_BACK) {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                               FROM_LEFT_2ND_BUTTON_PRESSED |
                               FROM_LEFT_3RD_BUTTON_PRESSED |
                               FROM_LEFT_4TH_BUTTON_PRESSED;
        if ((mouse.buttonState & backMask) != 0) {
          running = false;
          break;
        }
      } else if (ev.type == InputEvent::Type::Resize) {
        lastInitDraw = std::chrono::steady_clock::time_point::min();
      }
    }
  }

  if (!running) {
    player.close();
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return true;
  }
  if (!player.initOk()) {
    player.close();
    std::string initError = player.initError();
    if (initError.rfind("No video stream found", 0) == 0) {
      if (!enableAudio) {
        bool ok = reportVideoError("No video stream found.",
                                   "Audio playback is disabled.");
        finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
        return ok;
      }
      bool playAudio = showAudioFallbackPrompt(
          "No video stream found.", "This file can be played as audio only.");
      finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
      return playAudio ? false : true;
    }
    if (initError.empty()) {
      initError = "Failed to open video.";
    }
    bool ok = reportVideoError(initError, "");
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  int sourceWidth = player.sourceWidth();
  int sourceHeight = player.sourceHeight();
  SubtitleManager subtitleManager;
  subtitleManager.loadForVideo(file);
  const bool hasSubtitles = subtitleManager.trackCount() > 0;
  std::atomic<bool> enableSubtitlesShared{hasSubtitles};
  perfLogAppendf(&perfLog, "subtitle_detect tracks=%zu active=%s",
                 subtitleManager.trackCount(),
                 subtitleManager.activeTrackLabel().c_str());

  screen.updateSize();
  int initScreenWidth = std::max(20, screen.width());
  int initScreenHeight = std::max(10, screen.height());

  auto computeTargetSizeForSource = [&](int width, int height, int srcW,
                                        int srcH, bool showStatusLine) {
    int headerLines = showStatusLine ? 1 : 0;
    const int footerLines = 0;
    int maxHeight = std::max(1, height - headerLines - footerLines);
    int maxOutW = std::max(1, width - 8);
    AsciiArtLayout fitted =
        fitAsciiArtLayout(srcW, srcH, maxOutW, std::max(1, maxHeight));
    int outW = fitted.width;
    int outH = fitted.height;
    int targetW = std::max(2, outW * 2);
    int targetH = std::max(4, outH * 4);
    if (targetW & 1) ++targetW;
    if (targetH & 1) ++targetH;
    if (srcW > 0) targetW = std::min(targetW, srcW & ~1);
    if (srcH > 0) targetH = std::min(targetH, srcH & ~1);
    targetW = std::max(2, targetW);
    targetH = std::max(4, targetH);

    const int kMaxDecodeWidth = 1024;
    const int kMaxDecodeHeight = 768;
    if (targetW > kMaxDecodeWidth || targetH > kMaxDecodeHeight) {
      double scaleW = static_cast<double>(kMaxDecodeWidth) / targetW;
      double scaleH = static_cast<double>(kMaxDecodeHeight) / targetH;
      double scale = std::min(scaleW, scaleH);
      targetW = static_cast<int>(std::lround(targetW * scale));
      targetH = static_cast<int>(std::lround(targetH * scale));
      targetW = std::min(targetW, kMaxDecodeWidth);
      targetH = std::min(targetH, kMaxDecodeHeight);
      targetW &= ~1;
      targetH &= ~1;
      targetW = std::max(2, targetW);
      targetH = std::max(4, targetH);
    }
    return std::pair<int, int>(targetW, targetH);
  };

  auto computeTargetSize = [&](int width, int height) {
    bool showStatusLine = !player.audioOk();
    return computeTargetSizeForSource(width, height, sourceWidth, sourceHeight,
                                      showStatusLine);
  };

  auto computeAsciiOutputSize = [&](int maxWidth, int maxHeight, int srcW,
                                    int srcH) {
    int maxOutW = std::max(1, maxWidth - 8);
    AsciiArtLayout fitted =
        fitAsciiArtLayout(srcW, srcH, maxOutW, std::max(1, maxHeight));
    return std::pair<int, int>(fitted.width, fitted.height);
  };

  int requestedTargetW = 0;
  int requestedTargetH = 0;
  auto requestTargetSize = [&](int width, int height) -> bool {
    auto [targetW, targetH] = computeTargetSize(width, height);
    if (targetW == requestedTargetW && targetH == requestedTargetH) {
      return false;
    }
    requestedTargetW = targetW;
    requestedTargetH = targetH;
    player.requestResize(targetW, targetH);
    return true;
  };

  if (enableAscii) {
    requestTargetSize(initScreenWidth, initScreenHeight);
  }

  bool audioOk = player.audioOk();
  bool audioStarting = player.audioStarting();
  bool redraw = true;
  bool renderFailed = false;
  std::string renderFailMessage;
  std::string renderFailDetail;
  bool forceRefreshArt = false;
  bool pendingResize = false;
  bool userPaused = false;
  bool localSeekRequested = false;
  std::atomic<bool> windowLocalSeekRequested{false};
  bool closeWindowRequested = false;
  bool useWindowPresenter = false;
  auto seekRequestTime = std::chrono::steady_clock::time_point::min();
  double pendingSeekTargetSec = -1.0;
  std::atomic<double> windowPendingSeekTargetSec{-1.0};
  auto lastSeekSentTime = std::chrono::steady_clock::time_point::min();
  double queuedSeekTargetSec = -1.0;
  bool seekQueued = false;
  constexpr auto kSeekThrottleInterval = std::chrono::milliseconds(50);
  bool ended = false;
  auto lastUiDbgLog = std::chrono::steady_clock::time_point::min();
  std::string lastUiDbgLine1;
  std::string lastUiDbgLine2;
  std::atomic<bool> windowThreadRunning{true};
  std::atomic<bool> windowThreadEnabled{false};
  std::atomic<bool> windowForcePresent{false};
  std::mutex windowPresentMutex;
  std::condition_variable windowPresentCv;

  AsciiArt art;
  GpuAsciiRenderer& gpuRenderer = sharedGpuRenderer();
  if (!g_windowEnabledInitialized) {
    g_windowEnabledPersistent = config.enableWindow;
    g_windowEnabledInitialized = true;
  }
  bool& windowEnabled = g_windowEnabledPersistent;
  g_videoWindow.SetVsync(true);
  if (g_videoWindow.IsOpen()) {
    g_videoWindow.ShowWindow(windowEnabled);
  }
  if (windowEnabled && !g_videoWindow.IsOpen()) {
    g_videoWindow.Open(1280, 720, "Radioify Output");
    g_videoWindow.ShowWindow(true);
  }
  windowThreadEnabled.store(windowEnabled, std::memory_order_relaxed);
  if (windowEnabled) {
    windowForcePresent.store(true, std::memory_order_relaxed);
    windowPresentCv.notify_one();
  }
  const bool allowAsciiCpuFallback = false;
  VideoFrame frameBuffer;
  VideoFrame* frame = &frameBuffer;
  bool haveFrame = false;
  int cachedWidth = -1;
  int cachedMaxHeight = -1;
  int cachedFrameWidth = -1;
  int cachedFrameHeight = -1;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;

  auto finalizeAudioStart = [&]() {
    if (!audioStarting || player.audioStarting()) return;
    audioOk = player.audioOk();
    audioStarting = false;
    perfLogAppendf(&perfLog, "audio_start ok=%d", audioOk ? 1 : 0);
    if (audioOk) {
      AudioPerfStats stats = audioGetPerfStats();
      if (stats.periodFrames > 0 && stats.periods > 0) {
        perfLogAppendf(
            &perfLog,
            "audio_device period_frames=%u periods=%u buffer_frames=%u rate=%u "
            "channels=%u using_ffmpeg=%d",
            stats.periodFrames, stats.periods, stats.bufferFrames,
            stats.sampleRate, stats.channels, stats.usingFfmpeg ? 1 : 0);
      }
    }
    redraw = true;
  };

  auto sendSeekRequest = [&](double targetSec) {
    int64_t targetUs =
        static_cast<int64_t>(std::llround(targetSec * 1000000.0));
    player.requestSeek(targetUs);
    pendingSeekTargetSec = targetSec;
    localSeekRequested = true;
    windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                     std::memory_order_relaxed);
    windowLocalSeekRequested.store(localSeekRequested,
                                   std::memory_order_relaxed);
    seekRequestTime = std::chrono::steady_clock::now();
    lastSeekSentTime = seekRequestTime;
    queuedSeekTargetSec = -1.0;
    seekQueued = false;
    forceRefreshArt = true;
    redraw = true;
    perfLogAppendf(&perfLog,
                   "seek_request target_sec=%.3f target_us=%lld",
                   targetSec, static_cast<long long>(targetUs));
  };

  auto queueSeekRequest = [&](double targetSec) {
    pendingSeekTargetSec = targetSec;
    localSeekRequested = true;
    windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                     std::memory_order_relaxed);
    windowLocalSeekRequested.store(localSeekRequested,
                                   std::memory_order_relaxed);
    seekRequestTime = std::chrono::steady_clock::now();
    queuedSeekTargetSec = targetSec;
    seekQueued = true;
    forceRefreshArt = true;
    redraw = true;
  };

  constexpr auto kProgressOverlayTimeout = std::chrono::milliseconds(1750);
  constexpr auto kProgressOverlayExtendedTimeout =
      std::chrono::milliseconds(2500);
  auto nowMs = []() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  std::atomic<int64_t> overlayUntilMs{0};
  auto triggerOverlay = [&]() {
    int64_t now = nowMs();
    // If paused/seeking/ended, extend the overlay timeout so user sees controls longer
    bool extended = false;
    if (userPaused) extended = true;
    if (player.isSeeking()) extended = true;
    // 'ended' may not be set yet at first calls; it's captured by reference and can be used
    // if present later
    // Use a longer duration for extended cases
    int64_t timeoutMs = extended
                            ? static_cast<int64_t>(
                                  kProgressOverlayExtendedTimeout.count())
                            : static_cast<int64_t>(
                                  kProgressOverlayTimeout.count());
    overlayUntilMs.store(now + timeoutMs, std::memory_order_relaxed);
    windowForcePresent.store(true, std::memory_order_relaxed);
    windowPresentCv.notify_one();
  };
  auto overlayVisible = [&]() {
    int64_t until = overlayUntilMs.load(std::memory_order_relaxed);
    if (until <= 0) {
      return false;
    }
    return nowMs() <= until;
  };
  const std::string windowTitle = toUtf8String(file.filename());
    enum class OverlayControlId {
      Radio,
      Hz50,
      AudioTrack,
      Subtitles,
    };
  struct OverlayControlSpec {
    OverlayControlId id = OverlayControlId::Radio;
    std::string normalText;
    std::string hoverText;
    std::string renderText;
    bool active = false;
    int width = 0;
    int charStart = 0;
  };
  std::atomic<int> overlayControlHover{-1};

  auto buildOverlayControlSpecs =
      [&](int hoverIndex, std::vector<OverlayControlSpec>& out) {
        out.clear();
        auto makeLabels = [](const std::string& text) {
          return std::pair<std::string, std::string>(" [" + text + "] ",
                                                     "[ " + text + " ]");
        };
        OverlayControlSpec radio{};
        radio.id = OverlayControlId::Radio;
        radio.active = audioIsRadioEnabled();
        auto radioLabels =
            makeLabels(radio.active ? "Radio: AM" : "Radio: Dry");
        radio.normalText = radioLabels.first;
        radio.hoverText = radioLabels.second;
        radio.width =
            std::max(utf8CodepointCount(radio.normalText),
                     utf8CodepointCount(radio.hoverText));
        out.push_back(radio);

        OverlayControlSpec hz50{};
        hz50.id = OverlayControlId::Hz50;
        hz50.active = audioIs50HzEnabled();
        auto hzLabels = makeLabels(hz50.active ? "50Hz: 50" : "50Hz: Auto");
        hz50.normalText = hzLabels.first;
        hz50.hoverText = hzLabels.second;
          hz50.width = std::max(utf8CodepointCount(hz50.normalText),
                                utf8CodepointCount(hz50.hoverText));
          out.push_back(hz50);

          OverlayControlSpec audioTrack{};
          audioTrack.id = OverlayControlId::AudioTrack;
          size_t audioTracks = player.audioTrackCount();
          std::string audioLabel = "Audio: N/A";
          if (audioTracks > 0 && audioOk) {
            std::string activeAudio = player.activeAudioTrackLabel();
            if (activeAudio.empty()) activeAudio = "N/A";
            if (utf8CodepointCount(activeAudio) > 14) {
              activeAudio = utf8Take(activeAudio, 14);
            }
            audioLabel = "Audio: " + activeAudio;
          }
          auto audioLabels = makeLabels(audioLabel);
          audioTrack.normalText = audioLabels.first;
          audioTrack.hoverText = audioLabels.second;
          audioTrack.active =
              audioOk && player.canCycleAudioTracks();
          audioTrack.width = std::max(utf8CodepointCount(audioTrack.normalText),
                                      utf8CodepointCount(audioTrack.hoverText));
          out.push_back(audioTrack);

          OverlayControlSpec subtitles{};
          subtitles.id = OverlayControlId::Subtitles;
          bool subtitlesEnabled = enableSubtitlesShared.load(std::memory_order_relaxed);
          subtitles.active = hasSubtitles;
          std::string subtitleLabel = "Subs: N/A";
          if (hasSubtitles) {
            if (!subtitlesEnabled) {
              subtitleLabel = "Subs: Off";
            } else {
              std::string activeSubtitle = subtitleManager.activeTrackLabel();
              if (activeSubtitle.empty()) activeSubtitle = "N/A";
              if (utf8CodepointCount(activeSubtitle) > 14) {
                activeSubtitle = utf8Take(activeSubtitle, 14);
              }
              subtitleLabel = "Subs: " + activeSubtitle;
            }
          }
          auto subtitleLabels = makeLabels(subtitleLabel);
          if (!hasSubtitles) {
            subtitleLabels = makeLabels("Subs: N/A");
          }
          subtitles.normalText = subtitleLabels.first;
          subtitles.hoverText = subtitleLabels.second;
          subtitles.width = std::max(utf8CodepointCount(subtitles.normalText),
                                     utf8CodepointCount(subtitles.hoverText));
          out.push_back(subtitles);

        int cursor = 0;
        for (size_t i = 0; i < out.size(); ++i) {
          auto& spec = out[i];
          if (i > 0) cursor += 2;  // gap
          spec.charStart = cursor;
          bool hovered = static_cast<int>(i) == hoverIndex;
          spec.renderText = hovered ? spec.hoverText : spec.normalText;
          int textWidth = utf8CodepointCount(spec.renderText);
          if (textWidth < spec.width) {
            spec.renderText.append(static_cast<size_t>(spec.width - textWidth),
                                   ' ');
          } else if (textWidth > spec.width) {
            spec.renderText = utf8Take(spec.renderText, spec.width);
          }
          cursor += spec.width;
        }
      };

  auto buildOverlayControlsText = [&](int hoverIndex) -> std::string {
    std::vector<OverlayControlSpec> specs;
    buildOverlayControlSpecs(hoverIndex, specs);
    std::string line;
    for (size_t i = 0; i < specs.size(); ++i) {
      if (i > 0) line += "  ";
      line += specs[i].renderText;
    }
    return line;
  };
  auto formatWindowOverlayTime = [&](double s) -> std::string {
    if (!(s >= 0.0) || !std::isfinite(s)) return "--:--";
    int total = static_cast<int>(std::llround(s));
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int sec = total % 60;
    char buf[64];
    if (h > 0)
      std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else
      std::snprintf(buf, sizeof(buf), "%02d:%02d", m, sec);
    return std::string(buf);
  };
  auto buildWindowOverlayTopLine = [&]() -> std::string {
    return windowTitle;
  };
  auto buildWindowOverlayProgressSuffix = [&]() -> std::string {
    double currentSec = 0.0;
    double totalSec = -1.0;
    int64_t clockUs = player.currentUs();
    if (clockUs > 0) {
      currentSec = static_cast<double>(clockUs) / 1000000.0;
    }
    int64_t durUs = player.durationUs();
    if (durUs > 0) {
      totalSec = static_cast<double>(durUs) / 1000000.0;
    } else if (player.audioOk()) {
      totalSec = audioGetTotalSec();
    }
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    double displaySec = currentSec;
    bool seekingOverlay =
        player.isSeeking() ||
        windowLocalSeekRequested.load(std::memory_order_relaxed);
    double pendingTarget =
        windowPendingSeekTargetSec.load(std::memory_order_relaxed);
    if (seekingOverlay && pendingTarget >= 0.0 && totalSec > 0.0 &&
        std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingTarget, 0.0, totalSec);
    }
    std::string status = "\xE2\x96\xB6";  // ▶
    bool audioFinished = audioOk && audioIsFinished();
    bool paused = audioOk ? audioIsPaused() : userPaused;
    if (audioFinished) {
      status = "\xE2\x96\xA0";  // ■
    } else if (paused) {
      status = "\xE2\x8F\xB8";  // ⏸
    }
    int volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
    float radioGain = audioGetRadioMakeup();
    char radioBuf[32];
    std::snprintf(radioBuf, sizeof(radioBuf), " RG:%.2fx", radioGain);
    std::string timeLabel = totalSec > 0.0
                                ? (formatWindowOverlayTime(displaySec) + " / " +
                                   formatWindowOverlayTime(totalSec))
                                : formatWindowOverlayTime(displaySec);
    std::string volStr = " Vol: " + std::to_string(volPct) + "%" + radioBuf;
    return timeLabel + " " + status + volStr;
  };

  auto terminalOverlayControlAt = [&](const MouseEvent& mouse) -> int {
    if (!overlayVisible()) return -1;
    std::vector<OverlayControlSpec> specs;
    buildOverlayControlSpecs(-1, specs);
    int controlsY = std::max(0, std::max(10, screen.height()) - 3);
    if (mouse.pos.Y != controlsY) return -1;
    int relX = mouse.pos.X - 1;
    if (relX < 0) return -1;
    for (size_t i = 0; i < specs.size(); ++i) {
      const auto& spec = specs[i];
      if (relX >= spec.charStart && relX < spec.charStart + spec.width) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  auto windowOverlayControlAt = [&](const MouseEvent& mouse) -> int {
    if (!overlayVisible()) return -1;
    int winW = std::max(1, g_videoWindow.GetWidth());
    int winH = std::max(1, g_videoWindow.GetHeight());

    std::vector<OverlayControlSpec> specs;
    buildOverlayControlSpecs(-1, specs);
    if (specs.empty()) return -1;

    std::string topLine = buildWindowOverlayTopLine();
    std::string controlsLine = buildOverlayControlsText(-1);
    int maxChars = std::max(utf8CodepointCount(topLine),
                            utf8CodepointCount(controlsLine));
    if (maxChars <= 0) return -1;
    std::string progressLine = buildWindowOverlayProgressSuffix();
    int linePxH =
        std::clamp(static_cast<int>(std::round(winH * 0.045f)), 12, 36);
    int lineCount = 1 + (controlsLine.empty() ? 0 : 1) +
                    (progressLine.empty() ? 0 : 1);
    int textPxH =
        std::clamp(lineCount * linePxH + std::max(0, lineCount - 1) * 2, 14, 96);
    int textPxW =
        std::clamp(static_cast<int>(std::lround(winW * 0.96)), 1, winW);
    int totalGlyphH = lineCount * 7 + (lineCount - 1);
    int maxScaleVert = std::max(1, textPxH / std::max(1, totalGlyphH));
    int maxScaleHoriz = std::max(1, textPxW / std::max(1, maxChars * 6));
    int maxCap = std::max(3, textPxH / 80);
    int scale = std::min({maxScaleVert, maxScaleHoriz, maxCap});
    if (scale < 1) scale = 1;
    int charAdvance = 6 * scale;
    float textHeightNorm = static_cast<float>(textPxH) / winH;
    float textWidthNorm = static_cast<float>(textPxW) / winW;
    float textLeftNorm = 0.02f;
    if (textLeftNorm + textWidthNorm > 1.0f) {
      textLeftNorm = std::max(0.0f, 1.0f - textWidthNorm);
    }
    float textTopNorm = 0.95f - textHeightNorm;
    int textLeftPx = static_cast<int>(std::round(textLeftNorm * winW));
    int textTopPx = static_cast<int>(std::round(textTopNorm * winH));
    int controlsLineIndex = controlsLine.empty() ? -1 : 1;
    if (controlsLineIndex < 0) return -1;
    int controlsY0 =
        textTopPx + 1 + controlsLineIndex * (7 + 1) * scale;
    int controlsY1 = controlsY0 + 7 * scale;
    if (mouse.pos.Y < controlsY0 || mouse.pos.Y >= controlsY1) return -1;
    for (size_t i = 0; i < specs.size(); ++i) {
      const auto& spec = specs[i];
      int x0 = textLeftPx + 1 + spec.charStart * charAdvance;
      int x1 = x0 + spec.width * charAdvance;
      if (mouse.pos.X >= x0 && mouse.pos.X < x1) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  auto cycleSubtitleSelection = [&]() -> bool {
    if (!hasSubtitles) return false;
    const bool enabled =
        enableSubtitlesShared.load(std::memory_order_relaxed);
    if (!enabled) {
      enableSubtitlesShared.store(true, std::memory_order_relaxed);
      return true;
    }

    const size_t count = subtitleManager.trackCount();
    if (count <= 1) {
      enableSubtitlesShared.store(false, std::memory_order_relaxed);
      return true;
    }
    const size_t index = subtitleManager.activeTrackIndex();
    if (index == static_cast<size_t>(-1) || index + 1 >= count) {
      enableSubtitlesShared.store(false, std::memory_order_relaxed);
      return true;
    }
    return subtitleManager.cycleLanguage();
  };

  auto executeOverlayControl = [&](int controlIndex) -> bool {
    std::vector<OverlayControlSpec> specs;
    buildOverlayControlSpecs(-1, specs);
    if (controlIndex < 0 ||
        controlIndex >= static_cast<int>(specs.size())) {
      return false;
    }
    const auto& spec = specs[static_cast<size_t>(controlIndex)];
      switch (spec.id) {
        case OverlayControlId::Radio:
          if (!audioOk) return false;
          audioToggleRadio();
          return true;
        case OverlayControlId::Hz50:
          if (!audioOk) return false;
          audioToggle50Hz();
          return true;
        case OverlayControlId::AudioTrack:
          if (!audioOk) return false;
          return player.cycleAudioTrack();
        case OverlayControlId::Subtitles:
          return cycleSubtitleSelection();
      }
      return false;
    };
  auto isBackMousePressed = [&](const MouseEvent& mouse) {
    const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                           FROM_LEFT_2ND_BUTTON_PRESSED |
                           FROM_LEFT_3RD_BUTTON_PRESSED |
                           FROM_LEFT_4TH_BUTTON_PRESSED;
    return (mouse.buttonState & backMask) != 0;
  };
  auto isWindowProgressHit = [&](const MouseEvent& mouse) {
    float winW = static_cast<float>(g_videoWindow.GetWidth());
    float winH = static_cast<float>(g_videoWindow.GetHeight());
    if (winW <= 0.0f || winH <= 0.0f) {
      return false;
    }
    float mouseWinX = static_cast<float>(mouse.pos.X) / winW;
    float mouseWinY = static_cast<float>(mouse.pos.Y) / winH;
    const float barYTop = 0.95f;
    const float barYBot = 1.0f;
    const float barXLeft = 0.02f;
    const float barXRight = 0.98f;
    return mouseWinX >= barXLeft && mouseWinX <= barXRight &&
           mouseWinY >= barYTop && mouseWinY <= barYBot;
  };
  auto isTerminalProgressHit = [&](const MouseEvent& mouse) {
    int screenHeight = std::max(10, screen.height());
    int barY = progressBarY >= 0 ? progressBarY : (screenHeight - 1);
    if (mouse.pos.Y != barY) {
      return false;
    }
    if (progressBarWidth > 0 && progressBarX >= 0) {
      return mouse.pos.X >= progressBarX &&
             mouse.pos.X < progressBarX + progressBarWidth;
    }
    return mouse.pos.X >= 0;
  };
  auto isProgressHit = [&](const MouseEvent& mouse) {
    bool windowEvent = (mouse.control & 0x80000000) != 0;
    return windowEvent ? isWindowProgressHit(mouse)
                       : isTerminalProgressHit(mouse);
  };

  auto getSubtitleText = [&](int64_t clockUs, bool seeking) -> std::string {
    bool subtitlesEnabled =
        enableSubtitlesShared.load(std::memory_order_relaxed);
    if (!subtitlesEnabled || seeking || clockUs < 0 || !hasSubtitles) {
      return {};
    }
    const SubtitleTrack* activeTrack = subtitleManager.activeTrack();
    if (!activeTrack) {
      return {};
    }
    const SubtitleCue* cue = activeTrack->cueAt(clockUs);
    if (!cue) return {};
    return cue->text;
  };

  auto buildWindowUiState = [&]() {
    WindowUiState ui;
    double currentSec = 0.0;
    double totalSec = -1.0;
    int64_t clockUs = player.currentUs();
    if (clockUs > 0) {
      currentSec = static_cast<double>(clockUs) / 1000000.0;
    }
    int64_t durUs = player.durationUs();
    if (durUs > 0) {
      totalSec = static_cast<double>(durUs) / 1000000.0;
    } else if (player.audioOk()) {
      totalSec = audioGetTotalSec();
    }
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    double displaySec = currentSec;
    bool seekingOverlay =
        player.isSeeking() ||
        windowLocalSeekRequested.load(std::memory_order_relaxed);
    double pendingTarget =
        windowPendingSeekTargetSec.load(std::memory_order_relaxed);
    if (seekingOverlay && pendingTarget >= 0.0 && totalSec > 0.0 &&
        std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingTarget, 0.0, totalSec);
    }

    double ratio = 0.0;
    if (totalSec > 0.0 && std::isfinite(totalSec)) {
      ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
    }

    ui.progress = static_cast<float>(ratio);
    ui.isPaused = player.state() == PlayerState::Paused;
    ui.overlayAlpha = overlayVisible() ? 1.0f : 0.0f;
    ui.title = windowTitle;
    int hoverIndex = overlayControlHover.load(std::memory_order_relaxed);
    std::vector<OverlayControlSpec> controlSpecs;
    buildOverlayControlSpecs(hoverIndex, controlSpecs);
    ui.controls = buildOverlayControlsText(hoverIndex);
    ui.progressSuffix = buildWindowOverlayProgressSuffix();
    ui.controlButtons.clear();
    ui.controlButtons.reserve(controlSpecs.size());
    for (size_t i = 0; i < controlSpecs.size(); ++i) {
      WindowUiState::ControlButton btn;
      btn.text = controlSpecs[i].renderText;
      btn.active = controlSpecs[i].active;
      btn.hovered = static_cast<int>(i) == hoverIndex;
      ui.controlButtons.push_back(std::move(btn));
    }
    ui.displaySec = displaySec;
    ui.totalSec = totalSec;
    ui.volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
    std::string subtitle = getSubtitleText(clockUs, seekingOverlay);
    ui.subtitle = subtitle;
    ui.subtitleAlpha = subtitle.empty() ? 0.0f : 1.0f;
    return ui;
  };

  std::thread windowPresentThread([&]() {
    VideoFrame localFrame;
    uint64_t lastCounter = player.videoFrameCounter();
    while (windowThreadRunning.load(std::memory_order_relaxed)) {
      if (!windowThreadEnabled.load(std::memory_order_relaxed)) {
        std::unique_lock<std::mutex> lock(windowPresentMutex);
        windowPresentCv.wait_for(lock, std::chrono::milliseconds(50), [&]() {
          return !windowThreadRunning.load(std::memory_order_relaxed) ||
                 windowThreadEnabled.load(std::memory_order_relaxed);
        });
        continue;
      }

      if (!g_videoWindow.IsOpen() || !g_videoWindow.IsVisible()) {
        std::unique_lock<std::mutex> lock(windowPresentMutex);
        windowPresentCv.wait_for(lock, std::chrono::milliseconds(50));
        continue;
      }

      bool waitedForFrame = false;
      bool shouldWaitForFrame =
          !windowForcePresent.load(std::memory_order_relaxed) &&
          !overlayVisible() && !player.isSeeking();
      if (shouldWaitForFrame) {
        waitedForFrame = true;
        player.waitForVideoFrame(lastCounter, 16);
        if (!windowThreadRunning.load(std::memory_order_relaxed)) {
          break;
        }
      }

      uint64_t counterNow = player.videoFrameCounter();
      bool frameChanged = false;
      if (counterNow != lastCounter) {
        frameChanged = player.tryGetVideoFrame(&localFrame);
        lastCounter = counterNow;
      }
      if (!windowThreadRunning.load(std::memory_order_relaxed)) {
        break;
      }
      if (frameChanged) {
        if (localFrame.format != VideoPixelFormat::HWTexture ||
            !localFrame.hwTexture) {
          frameChanged = false;
        } else {
          D3D11_TEXTURE2D_DESC desc{};
          localFrame.hwTexture->GetDesc(&desc);
          bool is10Bit = (desc.Format == DXGI_FORMAT_P010);

          ID3D11Device* device = getSharedGpuDevice();
          if (device) {
            Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
            device->GetImmediateContext(&context);
            if (context) {
              std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
              bool updated = g_frameCache.Update(
                  device, context.Get(), localFrame.hwTexture.Get(),
                  localFrame.hwTextureArrayIndex, localFrame.width,
                  localFrame.height, localFrame.fullRange, localFrame.yuvMatrix,
                  localFrame.yuvTransfer, is10Bit ? 10 : 8,
                  localFrame.rotationQuarterTurns);
              if (!updated) {
                frameChanged = false;
              }
            }
          }
        }
      }

      WindowUiState ui = buildWindowUiState();
      bool overlayVisibleNow = ui.overlayAlpha > 0.01f;
      bool forcePresent =
          windowForcePresent.exchange(false, std::memory_order_relaxed);
      bool needsPresent =
          frameChanged || forcePresent || overlayVisibleNow || player.isSeeking();

      if (needsPresent) {
        if (!windowThreadRunning.load(std::memory_order_relaxed)) {
          break;
        }
        g_frameCache.WaitForFrameLatency(
            16, g_videoWindow.GetFrameLatencyWaitableObject());
        if (!windowThreadRunning.load(std::memory_order_relaxed)) {
          break;
        }
        if (frameChanged) {
          g_videoWindow.Present(g_frameCache, ui, true);
        } else {
          g_videoWindow.PresentOverlay(g_frameCache, ui, true);
        }
      } else if (!waitedForFrame) {
        std::unique_lock<std::mutex> lock(windowPresentMutex);
        windowPresentCv.wait_for(lock, std::chrono::milliseconds(10));
      }
    }
  });
  auto stopWindowThread = [&]() {
    windowThreadEnabled.store(false, std::memory_order_relaxed);
    windowForcePresent.store(false, std::memory_order_relaxed);
    windowThreadRunning.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    if (windowPresentThread.joinable()) {
      windowPresentThread.join();
    }
    if (closeWindowRequested && g_videoWindow.IsOpen()) {
      g_videoWindow.Close();
    }
  };

  auto maybeLogUiDbg = [&](const std::string& line1, const std::string& line2) {
    if (line1.empty() && line2.empty()) return;
    auto now = std::chrono::steady_clock::now();
    constexpr auto kUiDbgLogInterval = std::chrono::milliseconds(250);
    bool intervalOk =
        (lastUiDbgLog == std::chrono::steady_clock::time_point::min()) ||
        (now - lastUiDbgLog >= kUiDbgLogInterval);
    if (!intervalOk) return;
    bool changed = (line1 != lastUiDbgLine1) || (line2 != lastUiDbgLine2);
    if (!changed) return;
    lastUiDbgLine1 = line1;
    lastUiDbgLine2 = line2;
    lastUiDbgLog = now;
    if (!line1.empty()) {
      perfLogAppendf(&perfLog, "ui_dbg1 %s", line1.c_str());
    }
    if (!line2.empty()) {
      perfLogAppendf(&perfLog, "ui_dbg2 %s", line2.c_str());
    }
  };

  auto playbackMode = [&]() { return resolvePlaybackMode(enableAscii, windowEnabled); };

  auto renderScreen = [&](bool clearHistory, bool frameChanged) {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    if (!overlayVisible()) {
      overlayControlHover.store(-1, std::memory_order_relaxed);
    }
    std::string statusLine;
    if (!audioOk && !audioStarting) {
      statusLine = enableAudio ? "Audio unavailable" : "Audio disabled";
    }
    std::string subtitleText;
    std::string debugLine1;
    std::string debugLine2;
#if RADIOIFY_ENABLE_TIMING_LOG
    if (config.debugOverlay) {
      PlayerDebugInfo dbg = player.debugInfo();
      char buf1[256];
      char buf2[256];
      double masterSec =
          static_cast<double>(dbg.masterClockUs) / 1000000.0;
      double diffMs = static_cast<double>(dbg.lastDiffUs) / 1000.0;
      double delayMs = static_cast<double>(dbg.lastDelayUs) / 1000.0;
      std::snprintf(
          buf1, sizeof(buf1),
          "DBG state=%s serial=%d seek=%d qv=%zu master=%s %.3fs diff=%.1fms delay=%.1fms",
          playerStateLabel(dbg.state), dbg.currentSerial,
          dbg.pendingSeekSerial, dbg.videoQueueDepth,
          clockSourceLabel(dbg.masterSource), masterSec, diffMs, delayMs);
      std::snprintf(
          buf2, sizeof(buf2),
          "DBG audio ok=%d ready=%d fresh=%d starved=%d buf=%zuf rate=%u clock=%.3fs",
          dbg.audioOk ? 1 : 0, dbg.audioClockReady ? 1 : 0,
          dbg.audioClockFresh ? 1 : 0, dbg.audioStarved ? 1 : 0,
          dbg.audioBufferedFrames, dbg.audioSampleRate,
          static_cast<double>(dbg.audioClockUs) / 1000000.0);
      debugLine1 = buf1;
      debugLine2 = buf2;
      // maybeLogUiDbg(debugLine1, debugLine2); // REMOVED spammy UI logs
    }
#endif
    int headerLines = 0;
    if (!debugLine1.empty()) {
      headerLines += 1;
    }
    if (!debugLine2.empty()) {
      headerLines += 1;
    }
    if (!statusLine.empty()) {
      headerLines += 1;
    }
    const int footerLines = 0;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    double currentSec = 0.0;
    double totalSec = -1.0;
    int64_t clockUs = player.currentUs();
    if (clockUs > 0) {
      currentSec = static_cast<double>(clockUs) / 1000000.0;
    }
    int64_t durUs = player.durationUs();
    if (durUs > 0) {
      totalSec = static_cast<double>(durUs) / 1000000.0;
    } else if (audioOk) {
      totalSec = audioGetTotalSec();
    }
    if (totalSec > 0.0) {
      currentSec = std::clamp(currentSec, 0.0, totalSec);
    }
    double displaySec = currentSec;
    bool seekingOverlay = player.isSeeking() || localSeekRequested;
    if (seekingOverlay && totalSec > 0.0 && std::isfinite(totalSec)) {
      displaySec = std::clamp(pendingSeekTargetSec, 0.0, totalSec);
    }
    subtitleText = getSubtitleText(clockUs, seekingOverlay);

    bool waitingForAudio =
        audioOk && !audioStreamClockReady() && !audioIsFinished();
    bool audioStarved = audioOk && audioStreamStarved();
    bool waitingForVideo = !player.hasVideoFrame();
    bool isPaused = player.state() == PlayerState::Paused;
    bool allowFrame = haveFrame && !useWindowPresenter;

    auto waitingLabel = [&]() -> std::string {
      if (seekingOverlay) return "Seeking...";
      if (isPaused) return "Paused";
      if (player.state() == PlayerState::Opening) return "Opening...";
      if (player.state() == PlayerState::Prefill) return "Prefilling...";
      if (waitingForAudio) return "Waiting for audio...";
      if (audioStarved || waitingForVideo) return "Buffering video...";
      return "Waiting for video...";
    };

    const PlaybackRenderMode currentMode = playbackMode();
    bool sizeChanged = (width != cachedWidth || maxHeight != cachedMaxHeight ||
                        frame->width != cachedFrameWidth ||
                        frame->height != cachedFrameHeight);

    if (currentMode == PlaybackRenderMode::AsciiTerminal) {
      playback_frame_output::AsciiModePrepareInput asciiInput;
      asciiInput.allowFrame = allowFrame;
      asciiInput.clearHistory = clearHistory;
      asciiInput.frameChanged = frameChanged;
      asciiInput.sizeChanged = sizeChanged;
      asciiInput.allowAsciiCpuFallback = allowAsciiCpuFallback;
      asciiInput.width = width;
      asciiInput.maxHeight = maxHeight;
      asciiInput.computeAsciiOutputSize = computeAsciiOutputSize;
      asciiInput.frame = frame;
      asciiInput.art = &art;
      asciiInput.gpuRenderer = &gpuRenderer;
      asciiInput.frameCache = &g_frameCache;
      asciiInput.renderFailed = &renderFailed;
      asciiInput.renderFailMessage = &renderFailMessage;
      asciiInput.renderFailDetail = &renderFailDetail;
      asciiInput.haveFrame = &haveFrame;
      asciiInput.cachedWidth = &cachedWidth;
      asciiInput.cachedMaxHeight = &cachedMaxHeight;
      asciiInput.cachedFrameWidth = &cachedFrameWidth;
      asciiInput.cachedFrameHeight = &cachedFrameHeight;
      asciiInput.warningSink = [&](const std::string& message) {
        appendVideoWarning(message);
      };
      asciiInput.timingSink = [&](const std::string& message) {
        appendTiming(message);
      };
      playback_frame_output::prepareAsciiModeFrame(asciiInput);
    } else {
      playback_frame_output::prepareNonAsciiModeFrame(
          allowFrame, width, maxHeight, frame->width, frame->height,
          &cachedWidth, &cachedMaxHeight, &cachedFrameWidth,
          &cachedFrameHeight,
          [&](const std::string& message) { appendVideoWarning(message); },
          &haveFrame);
    }

    screen.clear(baseStyle);
    int headerY = 0;
    if (!debugLine1.empty()) {
      screen.writeText(0, headerY++, fitLine(debugLine1, width), dimStyle);
    }
    if (!debugLine2.empty()) {
      screen.writeText(0, headerY++, fitLine(debugLine2, width), dimStyle);
    }
    if (!statusLine.empty()) {
      screen.writeText(0, headerY++, fitLine(statusLine, width), dimStyle);
    }

    if (currentMode == PlaybackRenderMode::AsciiTerminal) {
      playback_frame_output::renderAsciiModeContent(
          screen, art, width, height, maxHeight, artTop, waitingLabel(),
          allowFrame, baseStyle, overlayVisible(), subtitleText, accentStyle,
          dimStyle);
    } else {
      playback_frame_output::renderNonAsciiModeContent(
          screen, windowEnabled, allowFrame, width, artTop, maxHeight, frame,
          player.sourceWidth(), player.sourceHeight(), dimStyle);
    }

    progressBarX = -1;
    progressBarY = -1;
    progressBarWidth = 0;
    if (overlayVisible()) {
      int barLine = height - 1;
      int suffixLine = barLine - 1;
      int controlsLine = suffixLine - 1;
      int titleLine = controlsLine - 1;
      if (controlsLine >= artTop && controlsLine >= 0) {
        std::vector<OverlayControlSpec> controls;
        buildOverlayControlSpecs(
            overlayControlHover.load(std::memory_order_relaxed), controls);
        for (size_t i = 0; i < controls.size(); ++i) {
          const auto& spec = controls[i];
          int x = 1 + spec.charStart;
          if (x >= width) break;
          int avail = width - x;
          if (avail <= 0) break;
          std::string text = spec.renderText;
          if (utf8CodepointCount(text) > avail) {
            text = utf8Take(text, avail);
          }
          Style style = spec.active ? accentStyle : baseStyle;
          bool hovered =
              static_cast<int>(i) ==
              overlayControlHover.load(std::memory_order_relaxed);
          if (hovered) {
            style = {style.bg, style.fg};
          }
          screen.writeText(x, controlsLine, text, style);
        }
      }
      if (titleLine >= artTop && titleLine >= 0) {
        std::string titleLineText = " " + buildWindowOverlayTopLine();
        screen.writeText(0, titleLine, fitLine(titleLineText, width),
                         accentStyle);
      }

      std::string suffix = buildWindowOverlayProgressSuffix();
      int barWidth = std::max(5, width - 2);
      double ratio = 0.0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
      }
      progressBarX = 1;
      progressBarY = barLine;
      progressBarWidth = barWidth;
      screen.writeChar(0, barLine, L'|', progressFrameStyle);
      auto barCells = renderProgressBarCells(ratio, barWidth,
                                             progressEmptyStyle, progressStart,
                                             progressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(1 + i, barLine, cell.ch, cell.style);
      }
      screen.writeChar(1 + barWidth, barLine, L'|', progressFrameStyle);
      if (!suffix.empty() && suffixLine >= artTop && suffixLine >= 0) {
        std::string suffixFit = fitLine(suffix, width);
        int suffixWidth = utf8CodepointCount(suffixFit);
        int suffixX = std::max(0, width - suffixWidth);
        screen.writeText(suffixX, suffixLine, suffixFit, baseStyle);
      }
    }

    screen.draw();
  };

  useWindowPresenter = windowThreadEnabled.load(std::memory_order_relaxed);
  renderScreen(true, true);
  if (renderFailed) {
    windowThreadRunning.store(false, std::memory_order_relaxed);
    windowThreadEnabled.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    stopWindowThread();
    player.close();
    if (audioOk || audioStarting) audioStop();
    g_frameCache.Reset();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    renderScreen(true, true);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  auto requestPlaybackExit = [&](bool quitApp) {
    running = false;
    closeWindowRequested = true;
    windowThreadEnabled.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    if (quitApp) {
      quitApplicationRequested = true;
    }
  };

  while (running) {
    finalizeAudioStart();

    if (g_videoWindow.IsOpen()) {
      g_videoWindow.PollEvents();
      if (windowEnabled && !g_videoWindow.IsVisible()) {
        windowEnabled = false;
        windowThreadEnabled.store(false, std::memory_order_relaxed);
        windowPresentCv.notify_one();
        forceRefreshArt = true;
        redraw = true;
      }
    }
    
    if (!running) break;

    // UI HEARTBEAT
    static auto lastUiHeartbeat = std::chrono::steady_clock::now();
    auto nowUi = std::chrono::steady_clock::now();
    if (nowUi - lastUiHeartbeat >= std::chrono::seconds(1)) {
        appendTimingFmt("video_heartbeat_ui redraw=%d seeker=%d paused=%d", 
                        redraw ? 1 : 0, localSeekRequested ? 1 : 0, userPaused ? 1 : 0);
        lastUiHeartbeat = nowUi;
    }

    InputEvent ev{};
    auto getNextEvent = [&]() {
        if (input.poll(ev)) return true;
        if (g_videoWindow.IsOpen() && g_videoWindow.PollInput(ev)) {
            // If it's a window mouse event in the bottom area, translate it to a seek
            if (ev.type == InputEvent::Type::Mouse && ev.mouse.buttonState == FROM_LEFT_1ST_BUTTON_PRESSED) {
                // In VideoWindow::WindowProc we ensure y > height * 0.9
                // We'll treat this as a ratio seek directly if possible, or fake a progress bar event.
                // Since handleInputEvent is complex, let's just handle it here.
                double ratio = (double)ev.mouse.pos.X / 1280.0; // This is a bit hacky as we don't know window width here
                // Wait, if I want it to be robust, I should pass the ratio in the event.
                // But InputEvent doesn't have a ratio field.
            }
            return true;
        }
        return false;
    };

    while (getNextEvent()) {
      if (!running) break;
      
      if (ev.type == InputEvent::Type::Resize) {
        pendingResize = true;
        redraw = true;
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        InputCallbacks cb;
        cb.onQuit = [&]() { requestPlaybackExit(true); };
        cb.onTogglePause = [&]() {
          if (audioOk) {
            audioTogglePause();
            userPaused = audioIsPaused();
          } else {
            userPaused = !userPaused;
            player.setVideoPaused(userPaused);
          }
        };
        cb.onToggleRadio = [&]() { if (audioOk) audioToggleRadio(); };
        cb.onToggle50Hz = [&]() { if (audioOk) audioToggle50Hz(); };
        cb.onSeekBy = [&](int dir) {
          double currentSec = player.currentUs() / 1000000.0;
          sendSeekRequest(currentSec + dir * 5.0);
        };
        cb.onAdjustVolume = [&](float delta) { audioAdjustVolume(delta); };
        cb.onAdjustRadioMakeup = [&](float delta) {
          audioAdjustRadioMakeup(delta);
          triggerOverlay();
          redraw = true;
          if (windowEnabled) {
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          }
        };

        if (ev.key.vk == 'W') {
          windowEnabled = !windowEnabled;
          if (windowEnabled) {
            if (!g_videoWindow.IsOpen()) {
              g_videoWindow.Open(1280, 720, "Radioify Output");
            }
            g_videoWindow.ShowWindow(true);
            windowThreadEnabled.store(true, std::memory_order_relaxed);
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          } else {
            windowThreadEnabled.store(false, std::memory_order_relaxed);
            windowPresentCv.notify_one();
            if (g_videoWindow.IsOpen()) {
              g_videoWindow.ShowWindow(false);
            }
            forceRefreshArt = true;
          }
          triggerOverlay();
          redraw = true;
          continue;
        }

        if (ev.key.vk == VK_ESCAPE || ev.key.vk == VK_BROWSER_BACK ||
            ev.key.vk == VK_BACK) {
          requestPlaybackExit(false);
          continue;
        }
          if (ev.key.vk == 'S' || ev.key.vk == 's') {
            if (cycleSubtitleSelection()) {
              triggerOverlay();
              redraw = true;
              if (windowEnabled) {
              windowForcePresent.store(true, std::memory_order_relaxed);
              windowPresentCv.notify_one();
            }
          }
          continue;
          }
          if (ev.key.vk == 'L' || ev.key.vk == 'l') {
            if (cycleSubtitleSelection()) {
              triggerOverlay();
              redraw = true;
              if (windowEnabled) {
                windowForcePresent.store(true, std::memory_order_relaxed);
                windowPresentCv.notify_one();
              }
            }
            continue;
          }
          if (ev.key.vk == 'A' || ev.key.vk == 'a') {
            if (player.cycleAudioTrack()) {
              triggerOverlay();
              redraw = true;
              if (windowEnabled) {
                windowForcePresent.store(true, std::memory_order_relaxed);
                windowPresentCv.notify_one();
              }
            }
            continue;
          }

          if (handlePlaybackInput(ev, running, cb)) {
          triggerOverlay();
          redraw = true;
          if (windowEnabled) {
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          }
          continue;
        }
      }
      if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        if (isBackMousePressed(mouse)) {
          requestPlaybackExit(false);
          continue;
        }
        bool windowEvent = (mouse.control & 0x80000000) != 0;
        int controlHit =
            windowEvent ? windowOverlayControlAt(mouse)
                        : terminalOverlayControlAt(mouse);
        int previousHover = overlayControlHover.load(std::memory_order_relaxed);
        int nextHover = overlayVisible() ? controlHit : -1;
        if (nextHover != previousHover) {
          overlayControlHover.store(nextHover, std::memory_order_relaxed);
          redraw = true;
          if (windowEnabled) {
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          }
        }
        if (controlHit >= 0) {
          triggerOverlay();
        }

        bool progressHit = isProgressHit(mouse);
        if (progressHit) {
          triggerOverlay();
          redraw = true;
          if (windowEnabled) {
            windowForcePresent.store(true, std::memory_order_relaxed);
            windowPresentCv.notify_one();
          }
        }

        bool leftPressed =
            (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
        if (leftPressed && mouse.eventFlags == 0 && controlHit >= 0) {
          if (executeOverlayControl(controlHit)) {
            triggerOverlay();
            redraw = true;
            if (windowEnabled) {
              windowForcePresent.store(true, std::memory_order_relaxed);
              windowPresentCv.notify_one();
            }
          }
          continue;
        }
        
        if (leftPressed && windowEvent) {
          if (progressHit) {
            float winW = static_cast<float>(g_videoWindow.GetWidth());
            float winH = static_cast<float>(g_videoWindow.GetHeight());
            if (winW > 0.0f && winH > 0.0f) {
              float mouseWinX = static_cast<float>(mouse.pos.X) / winW;
              const float barXLeft = 0.02f;
              const float barXRight = 0.98f;
              double barWidth = static_cast<double>(barXRight - barXLeft);
              double relX = static_cast<double>(mouseWinX - barXLeft);
              double ratio = relX / barWidth;
              ratio = std::clamp(ratio, 0.0, 1.0);
              double totalSec = player.durationUs() / 1000000.0;
              if (totalSec > 0.0 && std::isfinite(totalSec)) {
                double target = ratio * totalSec;
                queueSeekRequest(target);
              }
            }
          }
          continue;
        }

        if (leftPressed &&
            (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
          if (progressBarWidth > 0 && mouse.pos.Y == progressBarY &&
              progressBarX >= 0) {
            int rel = mouse.pos.X - progressBarX;
            if (rel >= 0 && rel < progressBarWidth) {
              double denom =
                  static_cast<double>(std::max(1, progressBarWidth - 1));
              double ratio = static_cast<double>(rel) / denom;
              ratio = std::clamp(ratio, 0.0, 1.0);
              double totalSec = player.durationUs() / 1000000.0;
              if (totalSec > 0.0 && std::isfinite(totalSec)) {
                double targetSec = ratio * totalSec;
                queueSeekRequest(targetSec);
              }
              continue;
            }
          }
        }
      }
    }
    if (!running) break;

    finalizeAudioStart();

    if (seekQueued) {
      auto now = std::chrono::steady_clock::now();
      bool canSend =
          (lastSeekSentTime == std::chrono::steady_clock::time_point::min()) ||
          (now - lastSeekSentTime >= kSeekThrottleInterval);
      if (canSend) {
        sendSeekRequest(queuedSeekTargetSec);
      }
    }

    if (pendingResize) {
      screen.updateSize();
      int width = std::max(20, screen.width());
      int height = std::max(10, screen.height());
      if (isAsciiPlaybackMode(playbackMode())) {
        requestTargetSize(width, height);
      }
      pendingResize = false;
      redraw = true;
    }

    bool presented = false;
    VideoFrame nextFrame;
    useWindowPresenter = windowThreadEnabled.load(std::memory_order_relaxed);
    if (!useWindowPresenter && player.tryGetVideoFrame(&nextFrame)) {
      frameBuffer = std::move(nextFrame);
      haveFrame = true;
      presented = true;
      if (!windowEnabled) {
        redraw = true;
      }
    }
    if (!player.hasVideoFrame()) {
      haveFrame = false;
    } else if (useWindowPresenter) {
      haveFrame = true;
    }

    if (localSeekRequested && player.isSeeking()) {
      localSeekRequested = false;
      windowLocalSeekRequested.store(localSeekRequested,
                                     std::memory_order_relaxed);
    }
    if (localSeekRequested && !player.isSeeking()) {
      auto now = std::chrono::steady_clock::now();
      if (seekRequestTime != std::chrono::steady_clock::time_point::min() &&
          now - seekRequestTime > std::chrono::milliseconds(500) &&
          player.hasVideoFrame()) {
        localSeekRequested = false;
        windowLocalSeekRequested.store(localSeekRequested,
                                       std::memory_order_relaxed);
      }
    }
    if (!player.isSeeking() && !localSeekRequested) {
      pendingSeekTargetSec = -1.0;
      windowPendingSeekTargetSec.store(pendingSeekTargetSec,
                                       std::memory_order_relaxed);
    }

    if (player.isEnded()) {
      // Mark ended but keep the loop running so user can seek back without causing
      // the program to immediately exit. This matches ASCII renderer behavior.
      ended = true;
      userPaused = true;
      triggerOverlay();
      redraw = true;
      // do not set running = false here; let user exit explicitly or seek
    }

#if RADIOIFY_ENABLE_TIMING_LOG
    if (redraw || overlayVisible() || config.debugOverlay) {
#else
    if (redraw || overlayVisible()) {
#endif
      auto t0 = std::chrono::steady_clock::now();
      renderScreen(forceRefreshArt, presented);
      auto t1 = std::chrono::steady_clock::now();
      auto durMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
      if (durMs > 100) {
        appendTimingFmt("video_ui_draw_slow dur_ms=%lld", (long long)durMs);
      }
      
      if (renderFailed) {
        running = false;
        break;
      }
      redraw = false;
      forceRefreshArt = false;
    }

#if RADIOIFY_ENABLE_TIMING_LOG
    if (!redraw && !overlayVisible() && !config.debugOverlay) {
#else
    if (!redraw && !overlayVisible()) {
#endif
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }

  if (renderFailed) {
    windowThreadRunning.store(false, std::memory_order_relaxed);
    windowThreadEnabled.store(false, std::memory_order_relaxed);
    windowPresentCv.notify_one();
    stopWindowThread();
    player.close();
    if (audioOk || audioStarting) audioStop();
    g_frameCache.Reset();
    bool ok = reportVideoError(renderFailMessage, renderFailDetail);
    renderScreen(true, true);
    finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
    return ok;
  }

  stopWindowThread();
  player.close();
  if (audioOk || audioStarting) audioStop();
  g_frameCache.Reset();
  finalizeVideoPlayback(screen, fullRedrawEnabled, &perfLog);
  return true;
}
