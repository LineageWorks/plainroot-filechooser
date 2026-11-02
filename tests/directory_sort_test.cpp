#include "../src/directory_sort.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <iterator>
#include <string>
#include <vector>

using plainroot::filechooser::detail::DirectoryEntry;
using plainroot::filechooser::detail::DirectorySortOrder;
using plainroot::filechooser::detail::sort_directory_entries;

int main() {
  const std::array entries{
      DirectoryEntry{"file10.txt", PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE, {}},
      DirectoryEntry{"folder10", PLAINROOT_CORE_FS_ENTRY_DIRECTORY, {}},
      DirectoryEntry{"file2.txt", PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE, {}},
      DirectoryEntry{"folder2", PLAINROOT_CORE_FS_ENTRY_DIRECTORY, {}},
      DirectoryEntry{".hidden", PLAINROOT_CORE_FS_ENTRY_REGULAR_FILE, {}},
  };
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

  const std::vector<const DirectoryEntry *> with_null{nullptr, &entries[0]};
  assert(sort_directory_entries(with_null, DirectorySortOrder::name_ascending)
             .size() == 1U);
  return 0;
}
