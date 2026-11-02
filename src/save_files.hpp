#pragma once

#include <systemd/sd-bus.h>

#include <cstddef>
#include <string>
#include <vector>

namespace plainroot::filechooser::detail {

inline constexpr size_t kMaximumSaveFiles = 256U;

enum class SaveFilesStatus {
  success,
  invalid_directory,
  no_files,
  too_many_files,
  invalid_name,
  name_too_long,
  path_too_long,
  filesystem_error,
};

struct SaveFilesResolution {
  SaveFilesStatus status = SaveFilesStatus::no_files;
  std::vector<std::string> paths;
};

// Reads an XDG FileChooser `files` option (`aay`). Each item must be a
// NUL-terminated native filename component.
bool parse_portal_save_files(sd_bus_message *message,
                             std::vector<std::string> &filenames);

// Maps requested names to non-existing paths in the selected directory.
// Existing files and duplicate requested names receive a unique suffix.
SaveFilesResolution resolve_save_files(
    const std::string &directory,
    const std::vector<std::string> &filenames);

} // namespace plainroot::filechooser::detail
