#pragma once

#include <string>
#include <string_view>

namespace plainroot::filechooser::detail {

struct ParsedAcceptLabel {
  std::string display_label;
  std::string mnemonic_utf8;
};

// Uses GTK's underscore mnemonic convention: `_Open` displays as `Open` and
// `__` displays as a literal underscore. The first mnemonic character is
// returned lowercased for toolkit shortcut registration.
ParsedAcceptLabel parse_portal_accept_label(std::string_view label);

} // namespace plainroot::filechooser::detail
