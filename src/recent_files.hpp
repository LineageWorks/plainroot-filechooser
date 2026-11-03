#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace plainroot::filechooser::detail {

struct RecentFile {
  std::string path;
  std::string name;
  int64_t visited_unix_seconds = 0;
};

bool load_recent_files(std::string_view xbel_path,
                       std::vector<RecentFile> &files);
bool record_recent_file(std::string_view xbel_path, std::string_view path,
                        std::string_view application_id);

} // namespace plainroot::filechooser::detail
