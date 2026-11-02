#pragma once

#include <plainroot/storage/client.h>
#include <plainroot/toolkit/toolkit.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace plainroot::filechooser::detail {

struct StorageVolume {
  std::string id;
  std::string label;
  std::string device_node;
  std::string filesystem_type;
  std::string mount_id;
  std::string mount_target;
  uint64_t size_bytes = 0;
  bool mounted = false;
  bool read_only = false;
};

struct NetworkMount {
  std::string id;
  std::string target;
  std::string filesystem_type;
};

struct StorageCatalog {
  bool complete = false;
  uint64_t revision = 0;
  std::vector<StorageVolume> removable_volumes;
  std::vector<NetworkMount> network_mounts;
};

struct StorageOperationUpdate {
  uint32_t type = 0;
  uint32_t request_kind = 0;
  uint32_t operation_kind = PLAINROOT_STORAGE_OPERATION_UNKNOWN;
  uint32_t state = PLAINROOT_STORAGE_OPERATION_ACCEPTED;
  uint32_t error = PLAINROOT_STORAGE_OPERATION_ERROR_NONE;
  plainroot_storage_result_v1 result = PLAINROOT_STORAGE_OK;
};

struct StorageServiceError {
  plainroot_storage_result_v1 result = PLAINROOT_STORAGE_OK;
  int32_t native_code = 0;
};

class StorageBridge {
public:
  using CatalogHandler = std::function<void(StorageCatalog)>;
  using OperationHandler = std::function<void(StorageOperationUpdate)>;
  using ErrorHandler = std::function<void(StorageServiceError)>;

  StorageBridge(plainroot_toolkit_application *application,
                CatalogHandler catalog_handler,
                OperationHandler operation_handler, ErrorHandler error_handler);
  StorageBridge(const StorageBridge &) = delete;
  StorageBridge &operator=(const StorageBridge &) = delete;
  ~StorageBridge();

  bool ready() const;
  void stop();
  plainroot_storage_result_v1 mount_volume(const std::string &id,
                                           bool read_only);
  plainroot_storage_result_v1 unmount_mount(const std::string &id);
  plainroot_storage_result_v1 request_snapshot();

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace plainroot::filechooser::detail
