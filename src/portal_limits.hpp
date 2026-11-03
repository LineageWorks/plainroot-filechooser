#pragma once

#include <cstddef>
#include <cstring>
#include <string_view>

namespace plainroot::filechooser::detail {

inline constexpr size_t kMaximumPortalTextLength = 4096U;
inline constexpr size_t kMaximumNativePathBytes = 64U * 1024U;
inline constexpr size_t kMaximumTotalFilterPatterns = 1024U;

inline bool portal_text_within_limit(std::string_view text) noexcept {
  return text.size() <= kMaximumPortalTextLength;
}

inline bool another_filter_pattern_allowed(size_t current_total) noexcept {
  return current_total < kMaximumTotalFilterPatterns;
}

inline bool native_path_payload_valid(const void *data,
                                      size_t size) noexcept {
  if (data == nullptr || size < 2U || size > kMaximumNativePathBytes)
    return false;
  const auto *path = static_cast<const char *>(data);
  return path[0] == '/' && path[size - 1U] == '\0' &&
         std::memchr(path, '\0', size - 1U) == nullptr;
}

} // namespace plainroot::filechooser::detail
