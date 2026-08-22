#include "core/utf8.h"

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "utf8_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;
  const std::string valid =
      "Radioify caf\xC3\xA9 \xF0\x9F\x8E\xB5";
  const auto wide = utf8ToWideStrict(valid);
  ok &= expect(wide.has_value(), "valid UTF-8 must decode strictly");
  ok &= expect(wide && wideToUtf8Strict(*wide) == valid,
               "valid Unicode must round-trip through wchar_t");
  ok &= expect(isValidUtf8(valid), "valid UTF-8 must validate");

  const std::string embeddedNull("A\0B", 3);
  const auto wideWithNull = utf8ToWideStrict(embeddedNull);
  ok &= expect(wideWithNull && wideWithNull->size() == 3 &&
                   wideToUtf8Strict(*wideWithNull) == embeddedNull,
               "embedded nulls must be preserved by sized conversions");

  const std::string overlong("\xC0\x80", 2);
  const std::string surrogate("\xED\xA0\x80", 3);
  const std::string outOfRange("\xF4\x90\x80\x80", 4);
  const std::string truncated("\xE2\x82", 2);
  ok &= expect(!utf8ToWideStrict(overlong) &&
                   !utf8ToWideStrict(surrogate) &&
                   !utf8ToWideStrict(outOfRange) &&
                   !utf8ToWideStrict(truncated),
               "strict decoding must reject malformed Unicode");

  const std::wstring lossy = utf8ToWideLossy(std::string("\xE9", 1));
  ok &= expect(lossy.size() == 1 &&
                   static_cast<char32_t>(lossy.front()) == 0xfffd,
               "lossy decoding must replace invalid bytes, not widen them");

  std::wstring invalidWide;
  invalidWide.push_back(static_cast<wchar_t>(0xd800));
  ok &= expect(!wideToUtf8Strict(invalidWide),
               "strict encoding must reject an isolated surrogate");
  ok &= expect(wideToUtf8Lossy(invalidWide) == "\xEF\xBF\xBD",
               "lossy encoding must replace an isolated surrogate");

  return ok ? 0 : 1;
}
