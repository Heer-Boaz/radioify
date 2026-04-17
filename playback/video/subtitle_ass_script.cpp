#include "subtitle_ass_script.h"

#include <algorithm>
#include <cstdio>
#include <string_view>
#include <utility>

namespace {

bool containsAsciiInsensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  for (size_t offset = 0; offset + needle.size() <= haystack.size(); ++offset) {
    bool match = true;
    for (size_t i = 0; i < needle.size(); ++i) {
      char a = haystack[offset + i];
      char b = needle[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
      if (a != b) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

void normalizeAssNewlines(std::string* text) {
  if (!text) return;
  std::string normalized;
  normalized.reserve(text->size());
  for (size_t i = 0; i < text->size(); ++i) {
    char ch = (*text)[i];
    if (ch == '\r') {
      if (i + 1 < text->size() && (*text)[i + 1] == '\n') {
        continue;
      }
      normalized.push_back('\n');
      continue;
    }
    normalized.push_back(ch);
  }
  *text = std::move(normalized);
}

std::string assTimestampFromUs(int64_t us) {
  if (us < 0) us = 0;
  const int64_t cs = us / 10000;
  const int64_t h = cs / 360000;
  const int64_t m = (cs / 6000) % 60;
  const int64_t s = (cs / 100) % 60;
  const int64_t cc = cs % 100;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld",
                static_cast<long long>(h), static_cast<long long>(m),
                static_cast<long long>(s), static_cast<long long>(cc));
  return std::string(buf);
}

std::string assNormalizeDialogueText(std::string text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    if (ch == '\r') continue;
    if (ch == '\n') {
      out += "\\N";
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

}  // namespace

void ensureEmbeddedAssScriptPreamble(std::string* script) {
  if (!script) return;
  normalizeAssNewlines(script);
  if (!script->empty() && script->back() != '\n') {
    script->push_back('\n');
  }
  if (!containsAsciiInsensitive(*script, "[Script Info]")) {
    script->append("[Script Info]\n");
    script->append("ScriptType: v4.00+\n");
    script->append("PlayResX: 384\n");
    script->append("PlayResY: 288\n");
  }
  if (!containsAsciiInsensitive(*script, "[V4+ Styles]") &&
      !containsAsciiInsensitive(*script, "[V4 Styles]")) {
    script->append("\n[V4+ Styles]\n");
    script->append(
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,"
        "OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,"
        "ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n");
    script->append(
        "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,"
        "0,0,0,0,100,100,0,0,1,2,0,2,12,12,12,1\n");
  }
  if (!containsAsciiInsensitive(*script, "[Events]")) {
    script->append("\n[Events]\n");
  }
  if (!containsAsciiInsensitive(
          *script,
          "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text") &&
      !containsAsciiInsensitive(
          *script,
          "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text")) {
    script->append(
        "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n");
  }
}

void appendEmbeddedAssDialogue(std::string* script, int64_t startUs,
                               int64_t endUs,
                               const AssPacketEventFields& event) {
  if (!script || event.text.empty()) return;
  if (endUs <= startUs) endUs = startUs + 100000;
  if (!script->empty() && script->back() != '\n') script->push_back('\n');

  script->append("Dialogue: ");
  script->append(std::to_string((std::max)(0, event.layer)));
  script->push_back(',');
  script->append(assTimestampFromUs(startUs));
  script->push_back(',');
  script->append(assTimestampFromUs(endUs));
  script->push_back(',');
  script->append(event.style.empty() ? "Default" : event.style);
  script->push_back(',');
  script->append(event.name);
  script->push_back(',');
  script->append(std::to_string((std::max)(0, event.marginL)));
  script->push_back(',');
  script->append(std::to_string((std::max)(0, event.marginR)));
  script->push_back(',');
  script->append(std::to_string((std::max)(0, event.marginV)));
  script->push_back(',');
  script->append(event.effect);
  script->push_back(',');
  script->append(assNormalizeDialogueText(event.text));
  script->push_back('\n');
}
