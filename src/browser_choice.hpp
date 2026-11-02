#pragma once

#include <string>
#include <vector>

namespace plainroot::filechooser {

struct BrowserChoiceOption {
  std::string id;
  std::string label;
};

struct BrowserChoice {
  std::string id;
  std::string label;
  std::vector<BrowserChoiceOption> options;
  std::string initial_selection;
};

struct BrowserChoiceSelection {
  std::string id;
  std::string value;
};

} // namespace plainroot::filechooser
