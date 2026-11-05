#include "recent_files.hpp"

#include "bookmarks.hpp"

#include <plainroot/core/filesystem.hpp>

#include <glib.h>

#include <algorithm>
#include <cerrno>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unordered_set>

namespace plainroot::filechooser::detail {
namespace {
constexpr uint64_t kMaximumRecentBytes = 4U * 1024U * 1024U;

bool local_path_for_uri(const char *uri, std::string &path) {
  gchar *host = nullptr;
  gchar *filename = g_filename_from_uri(uri, &host, nullptr);
  const bool local = host == nullptr || host[0] == '\0' ||
                     g_ascii_strcasecmp(host, "localhost") == 0;
  if (filename != nullptr && local && filename[0] == '/')
    path = canonical_local_path(filename);
  g_free(host);
  g_free(filename);
  return !path.empty();
}

std::string file_name(std::string_view path) {
  gchar *name = g_filename_display_basename(std::string(path).c_str());
  std::string result = name == nullptr ? std::string(path) : name;
  g_free(name);
  return result;
}
} // namespace

bool load_recent_files(std::string_view xbel_path,
                       std::vector<RecentFile> &files) {
  files.clear();
  if (xbel_path.empty() || xbel_path.front() != '/')
    return false;
  plainroot::core::filesystem::Buffer contents;
  const auto read = plainroot::core::filesystem::read_bytes(
      std::string(xbel_path), contents,
      {.maximum_bytes = kMaximumRecentBytes});
  if (read == PLAINROOT_CORE_FS_NOT_FOUND)
    return true;
  if (read != PLAINROOT_CORE_FS_OK)
    return false;
  GBookmarkFile *bookmarks = g_bookmark_file_new();
  GError *error = nullptr;
  const auto text = contents.text();
  if (!g_bookmark_file_load_from_data(bookmarks, text.data(), text.size(),
                                      &error)) {
    g_clear_error(&error);
    g_bookmark_file_free(bookmarks);
    return false;
  }
  gsize count = 0;
  gchar **uris = g_bookmark_file_get_uris(bookmarks, &count);
  std::unordered_set<std::string> seen;
  for (gsize i = 0; i < count && files.size() < 100U; ++i) {
    GError *private_error = nullptr;
    const gboolean is_private =
        g_bookmark_file_get_is_private(bookmarks, uris[i], &private_error);
    g_clear_error(&private_error);
    if (is_private)
      continue;
    std::string path;
    if (!local_path_for_uri(uris[i], path) || !seen.insert(path).second)
      continue;
    struct stat metadata {};
    if (stat(path.c_str(), &metadata) != 0 ||
        (!S_ISREG(metadata.st_mode) && !S_ISDIR(metadata.st_mode)))
      continue;
    GDateTime *visited = g_bookmark_file_get_visited_date_time(
        bookmarks, uris[i], nullptr);
    int64_t timestamp = visited == nullptr ? 0 : g_date_time_to_unix(visited);
    files.push_back({path, file_name(path), timestamp});
  }
  g_strfreev(uris);
  g_bookmark_file_free(bookmarks);
  std::stable_sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
    return a.visited_unix_seconds > b.visited_unix_seconds;
  });
  return true;
}

bool record_recent_file(std::string_view xbel_path, std::string_view input_path,
                        std::string_view application_id, bool allow_missing) {
  const std::string path = canonical_local_path(input_path);
  if (path.empty() || xbel_path.empty() || xbel_path.front() != '/')
    return false;
  struct stat metadata {};
  if (stat(path.c_str(), &metadata) != 0) {
    if (!allow_missing || errno != ENOENT)
      return false;
  } else if (!S_ISREG(metadata.st_mode) && !S_ISDIR(metadata.st_mode)) {
    return false;
  }
  GBookmarkFile *bookmarks = g_bookmark_file_new();
  plainroot::core::filesystem::Buffer contents;
  const auto read = plainroot::core::filesystem::read_bytes(
      std::string(xbel_path), contents,
      {.maximum_bytes = kMaximumRecentBytes});
  if (read == PLAINROOT_CORE_FS_OK) {
    GError *error = nullptr;
    const auto text = contents.text();
    if (!g_bookmark_file_load_from_data(bookmarks, text.data(), text.size(),
                                        &error)) {
      g_clear_error(&error);
      g_bookmark_file_free(bookmarks);
      return false;
    }
  } else if (read != PLAINROOT_CORE_FS_NOT_FOUND) {
    g_bookmark_file_free(bookmarks);
    return false;
  }
  gchar *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
  if (uri == nullptr) {
    g_bookmark_file_free(bookmarks);
    return false;
  }
  g_bookmark_file_set_mime_type(bookmarks, uri, "application/octet-stream");
  g_bookmark_file_set_is_private(bookmarks, uri, FALSE);
  GDateTime *now = g_date_time_new_now_utc();
  g_bookmark_file_set_visited_date_time(bookmarks, uri, now);
  g_date_time_unref(now);
  g_bookmark_file_add_application(bookmarks, uri,
                                  application_id.empty() ? "Plainroot FileChooser"
                                                         : std::string(application_id).c_str(),
                                  "gio open %u");
  gsize size = 0;
  GError *error = nullptr;
  gchar *serialized = g_bookmark_file_to_data(bookmarks, &size, &error);
  bool success = false;
  if (serialized != nullptr) {
    const size_t slash = xbel_path.find_last_of('/');
    const std::string file(xbel_path);
    const std::string directory = slash == 0 ? "/" : file.substr(0, slash);
    if (plainroot::core::filesystem::create_directories(directory) ==
        PLAINROOT_CORE_FS_OK) {
      plainroot::core::filesystem::WriteOptions options;
      options.file_mode = 0600U;
      success = plainroot::core::filesystem::write_utf8(
                    file, std::string_view(serialized, size), options) ==
                PLAINROOT_CORE_FS_OK;
    }
  }
  g_clear_error(&error);
  g_free(serialized);
  g_free(uri);
  g_bookmark_file_free(bookmarks);
  return success;
}
} // namespace plainroot::filechooser::detail
