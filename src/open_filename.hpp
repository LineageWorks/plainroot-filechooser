#pragma once

#include <string>

namespace plainroot::filechooser::detail {

enum class OpenFilenameStatus {
  valid,
  empty,
  invalid_component,
  too_long,
  invalid_encoding,
  not_found,
  directory,
  io_error,
};

struct OpenFilenameResolution {
  OpenFilenameStatus status = OpenFilenameStatus::empty;
  std::string path;
};

OpenFilenameResolution resolve_open_filename(const std::string &directory,
                                             const std::string &utf8_name);

} // namespace plainroot::filechooser::detail
