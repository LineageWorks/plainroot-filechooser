#include "../src/directory_sort.hpp"

#include <algorithm>
#include <cassert>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using plainroot::filechooser::detail::DirectoryEntry;
using plainroot::filechooser::detail::DirectorySortOrder;
using plainroot::filechooser::detail::sort_directory_entries;

int main() {
  const auto make_entry = [](std::string name,
                             plainroot_core_fs_entry_kind_v1 kind) {
    DirectoryEntry entry{};
    entry.name = std::move(name);
    entry.kind = kind;
    return entry;
  };
  std::vector<DirectoryEntry> entries{
      make_entry("file10.txt", PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE),
      make_entry("folder10", PLAINROOT_CORE_FS_ENTRY_DIRECTORY),
      make_entry("file2.txt", PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE),
      make_entry("folder2", PLAINROOT_CORE_FS_ENTRY_DIRECTORY),
      make_entry(".hidden", PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE),
  };
  const auto set_metadata = [&entries](size_t index, uint64_t size,
                                       int64_t modified,
                                       std::string type) {
    entries[index].metadata_available = true;
    entries[index].size_bytes = size;
    entries[index].modified_unix_seconds = modified;
    entries[index].mime_types = {std::move(type)};
  };
  set_metadata(0, 10, 300, "text/plain");
  set_metadata(1, 30, 150, "inode/directory");
  set_metadata(2, 2, 200, "image/png");
  set_metadata(3, 1, 100, "inode/directory");
  set_metadata(4, 1, 400, "application/octet-stream");

  std::vector<const DirectoryEntry *> input;
  for (const DirectoryEntry &entry : entries)
    input.push_back(&entry);

  const auto ascending =
      sort_directory_entries(input, DirectorySortOrder::name_ascending);
  assert(ascending.size() == entries.size());
  const auto position = [](const auto &sorted, const std::string &name) {
    const auto found = std::find_if(
        sorted.begin(), sorted.end(),
        [&name](const DirectoryEntry *entry) { return entry->name == name; });
    assert(found != sorted.end());
    return std::distance(sorted.begin(), found);
  };
  assert(position(ascending, "folder2") < position(ascending, "folder10"));
  assert(position(ascending, "folder10") < position(ascending, "file2.txt"));
  assert(position(ascending, "file2.txt") < position(ascending, "file10.txt"));

  const auto descending =
      sort_directory_entries(input, DirectorySortOrder::name_descending);
  assert(descending.size() == entries.size());
  assert(position(descending, "folder10") < position(descending, "folder2"));
  assert(position(descending, "folder2") < position(descending, "file10.txt"));
  assert(position(descending, "file10.txt") <
         position(descending, "file2.txt"));

  const auto smallest_first =
      sort_directory_entries(input, DirectorySortOrder::size_ascending);
  assert(position(smallest_first, "folder2") <
         position(smallest_first, "folder10"));
  assert(position(smallest_first, "folder10") <
         position(smallest_first, ".hidden"));
  assert(position(smallest_first, ".hidden") <
         position(smallest_first, "file2.txt"));
  assert(position(smallest_first, "file2.txt") <
         position(smallest_first, "file10.txt"));

  const auto largest_first =
      sort_directory_entries(input, DirectorySortOrder::size_descending);
  assert(position(largest_first, "folder10") <
         position(largest_first, "folder2"));
  assert(position(largest_first, "folder2") <
         position(largest_first, "file10.txt"));
  assert(position(largest_first, "file10.txt") <
         position(largest_first, "file2.txt"));

  const auto type_ascending =
      sort_directory_entries(input, DirectorySortOrder::type_ascending);
  assert(position(type_ascending, "folder2") <
         position(type_ascending, ".hidden"));
  assert(position(type_ascending, ".hidden") <
         position(type_ascending, "file2.txt"));
  assert(position(type_ascending, "file2.txt") <
         position(type_ascending, "file10.txt"));

  const auto type_descending =
      sort_directory_entries(input, DirectorySortOrder::type_descending);
  assert(position(type_descending, "file10.txt") <
         position(type_descending, "file2.txt"));
  assert(position(type_descending, "file2.txt") <
         position(type_descending, ".hidden"));

  auto extension_fallback_entries = entries;
  extension_fallback_entries[2].mime_types.clear();
  std::vector<const DirectoryEntry *> extension_fallback_input;
  for (const DirectoryEntry &entry : extension_fallback_entries)
    extension_fallback_input.push_back(&entry);
  const auto type_with_extension_fallback = sort_directory_entries(
      extension_fallback_input, DirectorySortOrder::type_ascending);
  assert(position(type_with_extension_fallback, "file10.txt") <
         position(type_with_extension_fallback, "file2.txt"));

  const auto newest_first = sort_directory_entries(
      input, DirectorySortOrder::modified_newest_first);
  assert(position(newest_first, "folder10") <
         position(newest_first, "folder2"));
  assert(position(newest_first, "folder2") <
         position(newest_first, ".hidden"));
  assert(position(newest_first, ".hidden") <
         position(newest_first, "file10.txt"));
  assert(position(newest_first, "file10.txt") <
         position(newest_first, "file2.txt"));

  const auto oldest_first = sort_directory_entries(
      input, DirectorySortOrder::modified_oldest_first);
  assert(position(oldest_first, "folder2") <
         position(oldest_first, "folder10"));
  assert(position(oldest_first, "folder10") <
         position(oldest_first, "file2.txt"));
  assert(position(oldest_first, "file2.txt") <
         position(oldest_first, "file10.txt"));
  assert(position(oldest_first, "file10.txt") <
         position(oldest_first, ".hidden"));

  auto recent_entries = entries;
  recent_entries[0].sort_timestamp = 20;
  recent_entries[1].sort_timestamp = 40;
  recent_entries[2].sort_timestamp = 10;
  std::vector<const DirectoryEntry *> recent_input;
  for (const DirectoryEntry &entry : recent_entries)
    recent_input.push_back(&entry);
  const auto recent =
      sort_directory_entries(recent_input, DirectorySortOrder::recent_first);
  assert(recent.size() == entries.size());
  assert(recent[0]->name == "folder10");
  assert(recent[1]->name == "file10.txt");
  assert(recent[2]->name == "file2.txt");

  const std::vector<const DirectoryEntry *> with_null{nullptr, &entries[0]};
  assert(sort_directory_entries(with_null, DirectorySortOrder::name_ascending)
             .size() == 1U);
  return 0;
}
