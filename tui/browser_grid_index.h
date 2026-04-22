#pragma once

#include "browser_model.h"

int browserGridEntryIndex(const GridLayout& layout,
                          BrowserState::ViewMode mode, int row, int col,
                          int count);

int browserGridVisibleCapacity(const GridLayout& layout);

int browserGridTotalRowsForEntryCount(BrowserState::ViewMode mode, int count,
                                      int rowsVisible, int cols);
