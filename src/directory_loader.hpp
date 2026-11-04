#pragma once

#include <plainroot/core/filesystem.h>
#include <plainroot/toolkit/toolkit.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plainroot::filechooser::detail {

struct DirectoryEntry {
  std::string name;
  plainroot_core_fs_entry_kind_v1 kind = PLAINROOT_CORE_FS_ENTRY_UNKNOWN;
  std::vector<std::string> mime_types;
  std::string path;
  int64_t sort_timestamp = 0;
};

struct DirectoryResult {
  enum class Operation {
    Listing,
    RecentListing,
    RecordRecent,
    CreateDirectory,
    InspectSymlink
  };

  uint64_t generation = 0;
  uint64_t request_id = 0;
  std::string path;
  plainroot_core_fs_result_v1 result = PLAINROOT_CORE_FS_IO_ERROR;
  int32_t native_code = 0;
  std::vector<DirectoryEntry> entries;
  bool mime_classified = false;
  bool mime_database_available = false;
  Operation operation = Operation::Listing;
  plainroot_core_fs_entry_kind_v1 inspected_kind =
      PLAINROOT_CORE_FS_ENTRY_UNKNOWN;
  bool selection_probe = false;
};

class DirectoryLoader {
public:
  using ResultHandler = std::function<void(DirectoryResult)>;

  DirectoryLoader(plainroot_toolkit_application *application,
                  ResultHandler result_handler);
  DirectoryLoader(const DirectoryLoader &) = delete;
  DirectoryLoader &operator=(const DirectoryLoader &) = delete;
  ~DirectoryLoader();

  bool ready() const;
  void request(std::string path, uint64_t generation,
               bool classify_mime = false);
  void request_recent(std::string xbel_path, uint64_t generation,
                      bool classify_mime = false);
  void record_recent(std::vector<std::string> paths, std::string xbel_path,
                     std::string application_id);
  void create_directory(std::string path, uint64_t generation,
                        uint64_t request_id);
  void inspect_symlink(std::string path, uint64_t generation,
                       bool selection_probe = false,
                       uint64_t request_id = 0);
  void stop();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser::detail
