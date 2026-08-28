#include "tui/ui/ui_viewport.h"

#include <cstdlib>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "ui_viewport_tests: " << message << '\n';
  return false;
}

}  // namespace

int main() {
  bool ok = true;

  const BrowserViewport normal =
      computeBrowserViewport(120, 30, true, false, 4, 5);
  ok &= expect(normal.width == 120 && normal.height == 30 &&
                   normal.searchBarY == 1 && normal.breadcrumbY == 2 &&
                   normal.headerLabelY == -1 && normal.listTop == 3 &&
                   normal.listHeight == 23,
               "a normal browser must preserve its complete chrome and "
               "footer reservation");

  const BrowserViewport labeled =
      computeBrowserViewport(120, 30, true, true, 0, 5);
  ok &= expect(labeled.headerLabelY == 2 && labeled.breadcrumbY == 3 &&
                   labeled.listTop == 4,
               "an optional header label must own a distinct row when it "
               "fits");

  const BrowserViewport shortViewport =
      computeBrowserViewport(20, 4, true, false, 4, 5);
  ok &= expect(shortViewport.width == 20 && shortViewport.height == 4 &&
                   shortViewport.searchBarY == 1 &&
                   shortViewport.breadcrumbY == 2 &&
                   shortViewport.headerLabelY == -1 &&
                   shortViewport.listTop == 3 &&
                   shortViewport.listHeight == 1,
               "a short viewport must publish its physical dimensions "
               "instead of a fictional minimum");

  const BrowserViewport threeRows =
      computeBrowserViewport(12, 3, true, false, 0, 5);
  ok &= expect(threeRows.width == 12 && threeRows.height == 3 &&
                   threeRows.searchBarY == 1 &&
                   threeRows.breadcrumbY == -1 &&
                   threeRows.listTop == 2 && threeRows.listHeight == 1,
               "three rows must retain search and content while dropping "
               "the breadcrumb");

  const BrowserViewport twoRows =
      computeBrowserViewport(8, 2, true, true, 0, 5);
  ok &= expect(twoRows.width == 8 && twoRows.height == 2 &&
                   twoRows.searchBarY == -1 &&
                   twoRows.breadcrumbY == -1 && twoRows.listTop == 1 &&
                   twoRows.listHeight == 1,
               "two rows must retain title and content without off-screen "
               "interactive chrome");

  const BrowserViewport oneRow =
      computeBrowserViewport(1, 1, true, true, 10, 5);
  ok &= expect(oneRow.width == 1 && oneRow.height == 1 &&
                   oneRow.headerLines == 0 && oneRow.listTop == 0 &&
                   oneRow.listHeight == 1,
               "the smallest viewport must remain truthful and usable");

  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
