#include "../src/portal_accept_label.hpp"

#include <iostream>
#include <string>

using plainroot::filechooser::detail::parse_portal_accept_label;

namespace {

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

} // namespace

int main() {
  const auto custom = parse_portal_accept_label("Choose _image");
  if (!require(custom.display_label == "Choose image" &&
                   custom.mnemonic_utf8 == "i",
               "removes a custom mnemonic marker and returns its key"))
    return 1;

  const auto escaped = parse_portal_accept_label("name__part");
  if (!require(escaped.display_label == "name_part" &&
                   escaped.mnemonic_utf8.empty(),
               "preserves escaped literal underscores"))
    return 1;

  const auto first_only = parse_portal_accept_label("_Open _files");
  if (!require(first_only.display_label == "Open files" &&
                   first_only.mnemonic_utf8 == "o",
               "uses only the first mnemonic marker"))
    return 1;

  const auto trailing = parse_portal_accept_label("Trailing_");
  if (!require(trailing.display_label == "Trailing_" &&
                   trailing.mnemonic_utf8.empty(),
               "keeps a trailing underscore literal"))
    return 1;

  const auto unicode = parse_portal_accept_label("_Éditer");
  if (!require(unicode.display_label == "Éditer" &&
                   unicode.mnemonic_utf8 == "é",
               "lowercases a Unicode mnemonic"))
    return 1;

  const std::string invalid_utf8{"bad\xfflabel", 9U};
  const auto invalid = parse_portal_accept_label(invalid_utf8);
  if (!require(invalid.display_label == invalid_utf8 &&
                   invalid.mnemonic_utf8.empty(),
               "passes malformed UTF-8 through without a mnemonic"))
    return 1;
  return 0;
}
