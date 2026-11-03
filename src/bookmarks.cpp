#include "bookmarks.hpp"

#include <plainroot/core/filesystem.hpp>

#include <glib.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace plainroot::filechooser::detail {
namespace {

constexpr uint64_t kMaximumBookmarkBytes = 1024U * 1024U;

std::string bookmarks_file_path() {
  const char *config_directory = g_get_user_config_dir();
  if (config_directory == nullptr || config_directory[0] != '/')
    return {};
  return std::string(config_directory) + "/gtk-3.0/bookmarks";
}

bool is_local_file_uri(std::string_view uri, std::string &path) {
  const std::string value(uri);
  gchar *hostname = nullptr;
  gchar *filename = g_filename_from_uri(value.c_str(), &hostname, nullptr);
  const bool local_host = hostname == nullptr || hostname[0] == '\0' ||
                          g_ascii_strcasecmp(hostname, "localhost") == 0;
  if (filename != nullptr && local_host && filename[0] == '/')
    path = canonical_local_path(filename);
  g_free(hostname);
  g_free(filename);
  return !path.empty();
}

bool valid_utf8(std::string_view value) {
  if (value.size() > static_cast<size_t>(G_MAXSSIZE))
    return false;
  return g_utf8_validate(value.data(), static_cast<gssize>(value.size()),
                         nullptr);
}

} // namespace

std::string canonical_local_path(std::string_view path) {
  if (path.empty() || path.front() != '/' ||
      path.find('\0') != std::string_view::npos)
    return {};
  const std::string value(path);
  gchar *canonical = g_canonicalize_filename(value.c_str(), nullptr);
  if (canonical == nullptr)
    return {};
  std::string result(canonical);
  g_free(canonical);
  return result;
}

bool read_gtk_bookmarks(std::vector<Bookmark> &bookmarks) {
  bookmarks.clear();
  const std::string path = bookmarks_file_path();
  if (path.empty())
    return false;

  plainroot::core::filesystem::Buffer contents;
  const auto result = plainroot::core::filesystem::read_bytes(
      path, contents, {.maximum_bytes = kMaximumBookmarkBytes});
  if (result == PLAINROOT_CORE_FS_NOT_FOUND)
    return true;
  if (result != PLAINROOT_CORE_FS_OK)
    return false;

  const std::string_view text = contents.text();
  size_t offset = 0;
  while (offset < text.size()) {
    const size_t newline = text.find('\n', offset);
    const size_t end = newline == std::string_view::npos ? text.size()
                                                          : newline;
    std::string_view line = text.substr(offset, end - offset);
    if (!line.empty() && line.back() == '\r')
      line.remove_suffix(1);
    if (!line.empty() && valid_utf8(line)) {
      const size_t separator = line.find(' ');
      const std::string_view uri = line.substr(0, separator);
      if (!uri.empty()) {
        Bookmark bookmark;
        bookmark.uri.assign(uri);
        if (separator != std::string_view::npos)
          bookmark.label.assign(line.substr(separator + 1U));
        (void)is_local_file_uri(uri, bookmark.path);
        bookmarks.push_back(std::move(bookmark));
      }
    }
    if (newline == std::string_view::npos)
      break;
    offset = newline + 1U;
  }
  return true;
}

bool write_gtk_bookmarks(const std::vector<Bookmark> &bookmarks) {
  const std::string path = bookmarks_file_path();
  if (path.empty())
    return false;

  std::string contents;
  for (const Bookmark &bookmark : bookmarks) {
    if (bookmark.uri.empty() || bookmark.uri.find_first_of("\r\n ") !=
                                    std::string::npos)
      continue;
    contents += bookmark.uri;
    if (!bookmark.label.empty() && valid_utf8(bookmark.label) &&
        bookmark.label.find_first_of("\r\n") == std::string::npos) {
      contents.push_back(' ');
      contents += bookmark.label;
    }
    contents.push_back('\n');
    if (contents.size() > kMaximumBookmarkBytes)
      return false;
  }

  const size_t separator = path.find_last_of('/');
  if (separator == std::string::npos || separator == 0U)
    return false;
  const std::string directory = path.substr(0, separator);
  if (plainroot::core::filesystem::create_directories(directory) !=
      PLAINROOT_CORE_FS_OK)
    return false;

  plainroot::core::filesystem::WriteOptions options;
  options.file_mode = 0600U;
  return plainroot::core::filesystem::write_utf8(path, contents, options) ==
         PLAINROOT_CORE_FS_OK;
}

bool make_local_bookmark(std::string_view path, Bookmark &bookmark) {
  const std::string canonical_path = canonical_local_path(path);
  if (canonical_path.empty())
    return false;
  gchar *uri = g_filename_to_uri(canonical_path.c_str(), nullptr, nullptr);
  if (uri == nullptr)
    return false;
  bookmark = {uri, {}, canonical_path};
  g_free(uri);
  return true;
}

std::string bookmark_display_name(const Bookmark &bookmark) {
  if (!bookmark.label.empty())
    return bookmark.label;
  if (bookmark.path.empty())
    return bookmark.uri;
  gchar *basename = g_filename_display_basename(bookmark.path.c_str());
  if (basename == nullptr)
    return bookmark.path;
  std::string result(basename);
  g_free(basename);
  return result;
}

} // namespace plainroot::filechooser::detail
