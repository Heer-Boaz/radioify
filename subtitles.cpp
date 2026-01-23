#include "subtitles.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace {

std::string trimLeft(const std::string& s) {
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  return s.substr(i);
}

std::string trimRight(const std::string& s) {
  if (s.empty()) return s;
  size_t i = s.size();
  while (i > 0 &&
         std::isspace(static_cast<unsigned char>(s[i - 1]))) {
    --i;
  }
  return s.substr(0, i);
}

std::string trim(const std::string& s) {
  return trimRight(trimLeft(s));
}

bool isAllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

bool parseInt(const std::string& s, int* out) {
  if (!out || s.empty()) return false;
  int val = 0;
  for (char c : s) {
    if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    val = val * 10 + (c - '0');
  }
  *out = val;
  return true;
}

std::string stripBom(const std::string& s) {
  if (s.size() >= 3 &&
      static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    return s.substr(3);
  }
  return s;
}

bool parseTimestampUs(const std::string& input, int64_t* outUs) {
  if (!outUs) return false;
  std::string s = trim(input);
  if (s.empty()) return false;

  std::vector<std::string> parts;
  std::string part;
  for (char c : s) {
    if (c == ':') {
      parts.push_back(part);
      part.clear();
    } else {
      part.push_back(c);
    }
  }
  parts.push_back(part);
  if (parts.size() < 2 || parts.size() > 3) return false;

  int h = 0;
  int m = 0;
  std::string secPart;
  if (parts.size() == 3) {
    if (!parseInt(parts[0], &h)) return false;
    if (!parseInt(parts[1], &m)) return false;
    secPart = parts[2];
  } else {
    if (!parseInt(parts[0], &m)) return false;
    secPart = parts[1];
  }

  std::string secStr = secPart;
  std::string msStr;
  size_t dot = secPart.find_first_of(".,");
  if (dot != std::string::npos) {
    secStr = secPart.substr(0, dot);
    msStr = secPart.substr(dot + 1);
  }

  int sec = 0;
  if (!parseInt(secStr, &sec)) return false;

  int ms = 0;
  if (!msStr.empty()) {
    int rawMs = 0;
    if (!parseInt(msStr, &rawMs)) return false;
    if (msStr.size() == 1) ms = rawMs * 100;
    else if (msStr.size() == 2) ms = rawMs * 10;
    else ms = rawMs;
    if (msStr.size() > 3) {
      int scale = 1;
      for (size_t i = 3; i < msStr.size(); ++i) scale *= 10;
      ms /= scale;
    }
  }

  int64_t totalMs =
      (static_cast<int64_t>(h) * 3600 +
       static_cast<int64_t>(m) * 60 +
       static_cast<int64_t>(sec)) *
          1000 +
      static_cast<int64_t>(ms);
  *outUs = totalMs * 1000;
  return true;
}

std::string stripTags(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool inTag = false;
  for (char c : s) {
    if (c == '<') {
      inTag = true;
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (!inTag) {
      out.push_back(c == '\t' ? ' ' : c);
    }
  }
  return out;
}

std::vector<std::string> splitLines(const std::string& s) {
  std::vector<std::string> lines;
  std::string line;
  for (char c : s) {
    if (c == '\r') continue;
    if (c == '\n') {
      lines.push_back(line);
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  lines.push_back(line);
  return lines;
}

std::string normalizeCueText(const std::string& s) {
  std::string stripped = stripTags(s);
  std::vector<std::string> lines = splitLines(stripped);
  std::string out;
  for (const std::string& line : lines) {
    std::string cleaned = trim(line);
    if (cleaned.empty()) continue;
    if (!out.empty()) out.push_back('\n');
    out += cleaned;
  }
  return out;
}

bool startsWithInsensitive(const std::string& s, const std::string& prefix) {
  if (s.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    char a = static_cast<char>(std::tolower(static_cast<unsigned char>(s[i])));
    char b = static_cast<char>(std::tolower(static_cast<unsigned char>(prefix[i])));
    if (a != b) return false;
  }
  return true;
}

}  // namespace

bool SubtitleTrack::loadFromFile(const std::filesystem::path& path,
                                 std::string* error) {
  clear();
  std::ifstream file(path);
  if (!file.is_open()) {
    if (error) {
      *error = "Failed to open subtitle file: " + path.string();
    }
    return false;
  }

  bool firstLine = true;
  bool inNote = false;
  bool haveTiming = false;
  SubtitleCue current{};
  std::string textBlock;
  std::string line;

  auto finishCue = [&]() {
    if (!haveTiming) return;
    std::string cleaned = normalizeCueText(textBlock);
    if (!cleaned.empty() && current.endUs >= current.startUs) {
      current.text = cleaned;
      cues_.push_back(current);
    }
    current = SubtitleCue{};
    textBlock.clear();
    haveTiming = false;
  };

  while (std::getline(file, line)) {
    if (firstLine) {
      line = stripBom(line);
      firstLine = false;
    }
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    std::string trimmed = trim(line);
    if (trimmed.empty()) {
      if (inNote) {
        inNote = false;
        continue;
      }
      finishCue();
      continue;
    }
    if (startsWithInsensitive(trimmed, "WEBVTT")) {
      continue;
    }
    if (startsWithInsensitive(trimmed, "NOTE")) {
      inNote = true;
      continue;
    }
    if (inNote) {
      continue;
    }

    if (!haveTiming) {
      if (isAllDigits(trimmed)) {
        continue;
      }
      size_t arrow = trimmed.find("-->");
      if (arrow == std::string::npos) {
        continue;
      }
      std::string startStr = trim(trimmed.substr(0, arrow));
      std::string endStr = trim(trimmed.substr(arrow + 3));
      size_t space = endStr.find(' ');
      if (space != std::string::npos) {
        endStr = trim(endStr.substr(0, space));
      }
      int64_t startUs = 0;
      int64_t endUs = 0;
      if (!parseTimestampUs(startStr, &startUs) ||
          !parseTimestampUs(endStr, &endUs)) {
        continue;
      }
      current.startUs = startUs;
      current.endUs = endUs;
      haveTiming = true;
      continue;
    }

    if (!textBlock.empty()) textBlock.push_back('\n');
    textBlock += line;
  }

  finishCue();

  std::sort(cues_.begin(), cues_.end(),
            [](const SubtitleCue& a, const SubtitleCue& b) {
              if (a.startUs == b.startUs) return a.endUs < b.endUs;
              return a.startUs < b.startUs;
            });

  return true;
}

void SubtitleTrack::clear() {
  cues_.clear();
}

const SubtitleCue* SubtitleTrack::cueAt(int64_t timeUs) const {
  if (cues_.empty()) return nullptr;
  auto it = std::upper_bound(
      cues_.begin(), cues_.end(), timeUs,
      [](int64_t t, const SubtitleCue& cue) { return t < cue.startUs; });
  if (it == cues_.begin()) return nullptr;
  --it;
  if (timeUs >= it->startUs && timeUs <= it->endUs) {
    return &(*it);
  }
  return nullptr;
}
