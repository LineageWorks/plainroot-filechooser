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
  constexpr char application_id[] = "org.plainroot.Editor";
  assert(record_recent_file(store, file, application_id));
  assert(load_recent_files(store, files));
  assert(files.size() == 1U);
  assert(files.front().path == file);
  assert(files.front().name == "note.txt");
  assert(files.front().visited_unix_seconds > 0);

  gchar *uri = g_filename_to_uri(file.c_str(), nullptr, &error);
  assert(uri != nullptr && error == nullptr);
  GBookmarkFile *recorded = g_bookmark_file_new();
  assert(g_bookmark_file_load_from_file(recorded, store.c_str(), &error));
  assert(error == nullptr);
  gsize application_count = 0;
  gchar **applications = g_bookmark_file_get_applications(
      recorded, uri, &application_count, &error);
  assert(applications != nullptr && error == nullptr);
  bool found_application = false;
  for (gsize index = 0; index < application_count; ++index)
    found_application |= std::string(applications[index]) == application_id;
  assert(found_application);
  g_strfreev(applications);
  g_bookmark_file_free(recorded);
  g_free(uri);

  const std::string save_target = directory + "/not-yet-created.txt";
  assert(!record_recent_file(store, save_target, application_id));
  assert(record_recent_file(store, save_target, application_id, true));
  assert(load_recent_files(store, files));
  assert(files.size() == 1U);
  assert(plainroot::core::filesystem::write_utf8(save_target, "saved") ==
         PLAINROOT_CORE_FS_OK);
  assert(load_recent_files(store, files));
  assert(files.size() == 2U);
  bool found_save_target = false;
  for (const RecentFile &recent : files)
    found_save_target |= recent.path == save_target;
  assert(found_save_target);

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
