#include "../src/recent_files.hpp"

#include <plainroot/core/filesystem.hpp>

#include <glib.h>

#include <cassert>
#include <string>
#include <vector>

using namespace plainroot::filechooser::detail;

int main() {
  GError *error = nullptr;
  gchar *temporary = g_dir_make_tmp("plainroot-recent-test-XXXXXX", &error);
  assert(temporary != nullptr && error == nullptr);
  const std::string directory(temporary);
  const std::string store = directory + "/recently-used.xbel";
  const std::string file = directory + "/note.txt";
  assert(plainroot::core::filesystem::write_utf8(file, "hello") ==
         PLAINROOT_CORE_FS_OK);

  std::vector<RecentFile> files;
  assert(load_recent_files(store, files));
  assert(files.empty());
  assert(record_recent_file(store, file, "plainroot-filechooser-test"));
  assert(load_recent_files(store, files));
  assert(files.size() == 1U);
  assert(files.front().path == file);
  assert(files.front().name == "note.txt");
  assert(files.front().visited_unix_seconds > 0);

  GBookmarkFile *bookmarks = g_bookmark_file_new();
  g_bookmark_file_set_is_private(bookmarks, "file:///tmp/private.txt", TRUE);
  g_bookmark_file_add_application(bookmarks, "file:///tmp/private.txt", "test", "true");
  g_bookmark_file_add_application(bookmarks, "file://remote.invalid/tmp/remote.txt", "test", "true");
  gsize size = 0;
  gchar *data = g_bookmark_file_to_data(bookmarks, &size, &error);
  assert(data != nullptr && error == nullptr);
  assert(plainroot::core::filesystem::write_utf8(
             store, std::string_view(data, size)) == PLAINROOT_CORE_FS_OK);
  g_free(data);
  g_bookmark_file_free(bookmarks);
  assert(load_recent_files(store, files));
  assert(files.empty());

  assert(plainroot::core::filesystem::write_utf8(store, "not xbel") ==
         PLAINROOT_CORE_FS_OK);
  assert(!record_recent_file(store, file, "test"));
  g_free(temporary);
  return 0;
}
