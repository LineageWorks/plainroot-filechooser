#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace plainroot::filechooser::detail {

inline bool trace_enabled() noexcept {
  static const bool enabled = [] {
    const char *value = std::getenv("PLAINROOT_FILECHOOSER_TRACE");
    return value != nullptr && std::strcmp(value, "1") == 0;
  }();
  return enabled;
}

inline void trace(const char *format, ...) noexcept {
  if (!trace_enabled())
    return;
  std::fputs("plainroot-filechooser-trace: ", stderr);
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
  std::fputc('\n', stderr);
}

} // namespace plainroot::filechooser::detail
