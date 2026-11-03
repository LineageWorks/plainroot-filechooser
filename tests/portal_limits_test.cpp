#include "../src/portal_limits.hpp"

#include <array>
#include <cassert>
#include <string>

using plainroot::filechooser::detail::kMaximumNativePathBytes;
using plainroot::filechooser::detail::kMaximumPortalTextLength;
using plainroot::filechooser::detail::kMaximumTotalFilterPatterns;
using plainroot::filechooser::detail::another_filter_pattern_allowed;
using plainroot::filechooser::detail::native_path_payload_valid;
using plainroot::filechooser::detail::portal_text_within_limit;

int main() {
  assert(portal_text_within_limit(""));
  assert(portal_text_within_limit(std::string(kMaximumPortalTextLength, 'x')));
  assert(!portal_text_within_limit(
      std::string(kMaximumPortalTextLength + 1U, 'x')));
  assert(another_filter_pattern_allowed(kMaximumTotalFilterPatterns - 1U));
  assert(!another_filter_pattern_allowed(kMaximumTotalFilterPatterns));

  constexpr std::array<char, 3> root_path{'/', '\0', '\0'};
  assert(native_path_payload_valid(root_path.data(), 2U));
  assert(!native_path_payload_valid(nullptr, root_path.size()));
  assert(!native_path_payload_valid(root_path.data(), 1U));
  constexpr std::array<char, 3> relative_path{'a', '\0', '\0'};
  assert(!native_path_payload_valid(relative_path.data(), 2U));
  constexpr std::array<char, 4> embedded_nul{'/', '\0', 'x', '\0'};
  assert(!native_path_payload_valid(embedded_nul.data(), embedded_nul.size()));
  constexpr std::array<char, 3> missing_terminator{'/', 't', 'm'};
  assert(!native_path_payload_valid(missing_terminator.data(),
                                    missing_terminator.size()));
  assert(!native_path_payload_valid(root_path.data(),
                                    kMaximumNativePathBytes + 1U));
  return 0;
}
