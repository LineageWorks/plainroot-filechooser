#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace plainroot::filechooser::detail {

struct Bookmark {
  // Keep the original URI so non-local GTK bookmarks survive a write even
  // though this browser can currently navigate only local paths.
  std::string uri;
  std::string label;
  std::string path;
};

bool read_gtk_bookmarks(std::vector<Bookmark> &bookmarks);
bool write_gtk_bookmarks(const std::vector<Bookmark> &bookmarks);
bool make_local_bookmark(std::string_view path, Bookmark &bookmark);
std::string canonical_local_path(std::string_view path);
std::string bookmark_display_name(const Bookmark &bookmark);

} // namespace plainroot::filechooser::detail
