#pragma once

#include <string>
#include <string_view>

namespace plainroot::filechooser::detail {

enum class LocationPathStatus {
  valid,
  empty,
  invalid_encoding,
  invalid_base,
  too_long,
};

struct LocationPathResolution {
  LocationPathStatus status = LocationPathStatus::empty;
  std::string path;
};

// Converts a user-entered UTF-8 location to a normalized native absolute path.
// Relative paths use current_directory; `~` and `~/...` use home_directory.
// This is lexical only and does not access the filesystem.
LocationPathResolution resolve_location_path(
    std::string_view current_directory, std::string_view utf8_location,
    std::string_view home_directory);

// Converts a native filesystem path for display/editing in a UTF-8 field.
std::string location_path_to_utf8(std::string_view native_path);

} // namespace plainroot::filechooser::detail
