#include "directory_sort.hpp"

#include <glib.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace plainroot::filechooser::detail {
namespace {

struct SortKey {
  const DirectoryEntry *entry = nullptr;
  std::string name;
  std::string type;
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

std::string make_type_collation_key(std::string_view type) {
  const std::string type_string(type);
  gchar *display = g_filename_display_name(type_string.c_str());
  const char *text = display == nullptr ? type_string.c_str() : display;
  gchar *folded = g_utf8_casefold(text, -1);
  gchar *key = folded == nullptr ? nullptr : g_utf8_collate_key(folded, -1);
  std::string result = key == nullptr ? text : key;
  g_free(key);
  g_free(folded);
  g_free(display);
  return result;
}

std::string make_type_key(const DirectoryEntry &entry) {
  if (entry.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY)
    return {};
  if (!entry.mime_types.empty())
    return make_type_collation_key(entry.mime_types.front());

  const size_t dot = entry.name.find_last_of('.');
  if (dot == std::string::npos || dot == 0U || dot + 1U == entry.name.size())
    return {};

  return make_type_collation_key(entry.name.substr(dot + 1U));
}

bool is_directory(const DirectoryEntry &entry) {
  return entry.kind == PLAINROOT_CORE_FS_ENTRY_DIRECTORY;
}

bool ascending_order(DirectorySortOrder order) {
  return order == DirectorySortOrder::name_ascending ||
         order == DirectorySortOrder::size_ascending ||
         order == DirectorySortOrder::type_ascending ||
         order == DirectorySortOrder::modified_oldest_first;
}

int compare_metadata_time(const DirectoryEntry &left,
                          const DirectoryEntry &right) {
  if (left.modified_unix_seconds != right.modified_unix_seconds)
    return left.modified_unix_seconds < right.modified_unix_seconds ? -1 : 1;
  if (left.modified_nanoseconds != right.modified_nanoseconds)
    return left.modified_nanoseconds < right.modified_nanoseconds ? -1 : 1;
  return 0;
}

int compare_names(const SortKey &left, const SortKey &right) {
  const int key_order =
      std::strcmp(left.name.c_str(), right.name.c_str());
  if (key_order != 0)
    return key_order;
  return left.entry->name.compare(right.entry->name);
}

} // namespace

std::vector<const DirectoryEntry *>
sort_directory_entries(const std::vector<const DirectoryEntry *> &entries,
                       DirectorySortOrder order) {
  std::vector<SortKey> keys;
  keys.reserve(entries.size());
  const bool sort_by_type = order == DirectorySortOrder::type_ascending ||
                            order == DirectorySortOrder::type_descending;
  for (const DirectoryEntry *entry : entries) {
    if (entry != nullptr)
      keys.push_back({entry, make_filename_key(entry->name),
                      sort_by_type ? make_type_key(*entry) : std::string{}});
  }

  const bool ascending = ascending_order(order);
  std::stable_sort(keys.begin(), keys.end(),
                   [ascending, order](const SortKey &left,
                                      const SortKey &right) {
                     if (order == DirectorySortOrder::recent_first &&
                         left.entry->sort_timestamp !=
                             right.entry->sort_timestamp)
                       return left.entry->sort_timestamp >
                              right.entry->sort_timestamp;

                     const bool left_directory = is_directory(*left.entry);
                     const bool right_directory = is_directory(*right.entry);
                     if (left_directory != right_directory)
                       return left_directory;

                     if (order == DirectorySortOrder::size_ascending ||
                         order == DirectorySortOrder::size_descending) {
                       if (left.entry->metadata_available !=
                           right.entry->metadata_available)
                         return left.entry->metadata_available;
                       if (left.entry->metadata_available &&
                           left.entry->size_bytes !=
                               right.entry->size_bytes) {
                         const bool less = left.entry->size_bytes <
                                           right.entry->size_bytes;
                         return ascending ? less : !less;
                       }
                     } else if (
                         order == DirectorySortOrder::type_ascending ||
                         order == DirectorySortOrder::type_descending) {
                       const int type_order =
                           left.type.compare(right.type);
                       if (type_order != 0)
                         return ascending ? type_order < 0 : type_order > 0;
                     } else if (
                         order == DirectorySortOrder::modified_newest_first ||
                         order == DirectorySortOrder::modified_oldest_first) {
                       if (left.entry->metadata_available !=
                           right.entry->metadata_available)
                         return left.entry->metadata_available;
                       if (left.entry->metadata_available) {
                         const int time_order = compare_metadata_time(
                             *left.entry, *right.entry);
                         if (time_order != 0)
                           return ascending ? time_order < 0
                                            : time_order > 0;
                       }
                     }

                     const int name_order = compare_names(left, right);
                     return order == DirectorySortOrder::name_descending
                                ? name_order > 0
                                : name_order < 0;
                   });

  std::vector<const DirectoryEntry *> sorted;
  sorted.reserve(keys.size());
  for (const SortKey &key : keys)
    sorted.push_back(key.entry);
  return sorted;
}

} // namespace plainroot::filechooser::detail
