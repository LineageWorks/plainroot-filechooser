#include "directory_sort.hpp"

#include <glib.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace plainroot::filechooser::detail {
namespace {

struct SortKey {
  const DirectoryEntry *entry = nullptr;
  std::string collation_key;
};

std::string make_filename_key(const std::string &name) {
  gchar *display_name = g_filename_display_name(name.c_str());
  const char *display = display_name == nullptr ? name.c_str() : display_name;
  gchar *key = g_utf8_collate_key_for_filename(display, -1);
  std::string result = key == nullptr ? display : key;
  g_free(key);
  g_free(display_name);
  return result;
}

bool is_directory(const DirectoryEntry &entry) {
  return entry.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
}

} // namespace

std::vector<const DirectoryEntry *>
sort_directory_entries(const std::vector<const DirectoryEntry *> &entries,
                       DirectorySortOrder order) {
  std::vector<SortKey> keys;
  keys.reserve(entries.size());
  for (const DirectoryEntry *entry : entries) {
    if (entry != nullptr)
      keys.push_back({entry, make_filename_key(entry->name)});
  }

  const bool ascending = order == DirectorySortOrder::name_ascending;
  std::stable_sort(keys.begin(), keys.end(),
                   [ascending](const SortKey &left, const SortKey &right) {
                     const bool left_directory = is_directory(*left.entry);
                     const bool right_directory = is_directory(*right.entry);
                     if (left_directory != right_directory)
                       return left_directory;
                     const int key_order =
                         std::strcmp(left.collation_key.c_str(),
                                     right.collation_key.c_str());
                     if (key_order != 0)
                       return ascending ? key_order < 0 : key_order > 0;
                     if (left.entry->name != right.entry->name)
                       return ascending ? left.entry->name < right.entry->name
                                        : left.entry->name > right.entry->name;
                     return false;
                   });

  std::vector<const DirectoryEntry *> sorted;
  sorted.reserve(keys.size());
  for (const SortKey &key : keys)
    sorted.push_back(key.entry);
  return sorted;
}

} // namespace plainroot::filechooser::detail
