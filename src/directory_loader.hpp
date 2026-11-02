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
};

struct DirectoryResult {
  uint64_t generation = 0;
  std::string path;
  plainroot_core_fs_result_v1 result = PLAINROOT_CORE_FS_IO_ERROR;
  int32_t native_code = 0;
  std::vector<DirectoryEntry> entries;
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
  void request(std::string path, uint64_t generation);
  void stop();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser::detail
