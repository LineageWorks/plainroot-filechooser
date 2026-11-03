#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace plainroot::filechooser {

struct BrowserFilterPattern {
  uint32_t type = 0;
  std::string pattern;
};

struct BrowserFilter {
  std::string name;
  std::vector<BrowserFilterPattern> patterns;
};

} // namespace plainroot::filechooser
