#include "storage_bridge.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <memory>
#include <new>
#include <poll.h>
#include <thread>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace plainroot::filechooser::detail {
namespace {

std::string copy_bytes(plainroot_storage_bytes_v1 value) {
  if (value.data == nullptr || value.size == 0)
    return {};
  return std::string(value.data, value.size);
}

struct BlockInfo {
  uint32_t flags = 0;
  uint32_t major = 0;
  uint32_t minor = 0;
  uint64_t size_bytes = 0;
  std::string id;
  std::string node;
};

struct MountInfo {
  uint32_t flags = 0;
  uint32_t major = 0;
  uint32_t minor = 0;
  std::string id;
  std::string target;
  std::string filesystem_type;
};

StorageCatalog copy_catalog(const plainroot_storage_client_snapshot *snapshot) {
  StorageCatalog output;
  if (snapshot == nullptr)
    return output;

  plainroot_storage_snapshot_info_v1 info{};
  info.struct_size = sizeof(info);
  if (plainroot_storage_client_snapshot_get_info_v1(snapshot, &info) !=
      PLAINROOT_STORAGE_OK)
    return output;
  output.complete = (info.flags & PLAINROOT_STORAGE_SNAPSHOT_PARTIAL) == 0;
  output.revision = info.revision;

  std::vector<BlockInfo> blocks;
  const size_t block_count =
      plainroot_storage_client_snapshot_block_device_count_v1(snapshot);
  blocks.reserve(block_count);
  for (size_t index = 0; index < block_count; ++index) {
    plainroot_storage_block_device_v1 block{};
    block.struct_size = sizeof(block);
    if (plainroot_storage_client_snapshot_get_block_device_v1(
            snapshot, index, &block) != PLAINROOT_STORAGE_OK)
      continue;
    blocks.push_back({block.flags, block.device_major, block.device_minor,
                      block.size_bytes, copy_bytes(block.id),
                      copy_bytes(block.device_node)});
  }

  std::vector<MountInfo> mounts;
  const size_t mount_count =
      plainroot_storage_client_snapshot_mount_count_v1(snapshot);
  mounts.reserve(mount_count);
  for (size_t index = 0; index < mount_count; ++index) {
    plainroot_storage_mount_v1 mount{};
    mount.struct_size = sizeof(mount);
    if (plainroot_storage_client_snapshot_get_mount_v1(
            snapshot, index, &mount) != PLAINROOT_STORAGE_OK)
      continue;
    MountInfo copied{mount.flags,
                     mount.device_major,
                     mount.device_minor,
                     copy_bytes(mount.id),
                     copy_bytes(mount.target),
                     copy_bytes(mount.filesystem_type)};
    if ((copied.flags & PLAINROOT_STORAGE_MOUNT_NETWORK) != 0 &&
        !copied.id.empty() && !copied.target.empty())
      output.network_mounts.push_back(
          {copied.id, copied.target, copied.filesystem_type});
    mounts.push_back(std::move(copied));
  }

  const size_t volume_count =
      plainroot_storage_client_snapshot_volume_count_v1(snapshot);
  output.removable_volumes.reserve(volume_count);
  for (size_t index = 0; index < volume_count; ++index) {
    plainroot_storage_volume_v1 volume{};
    volume.struct_size = sizeof(volume);
    if (plainroot_storage_client_snapshot_get_volume_v1(
            snapshot, index, &volume) != PLAINROOT_STORAGE_OK)
      continue;
    const std::string block_id = copy_bytes(volume.block_device_id);
    const auto block = std::find_if(
        blocks.begin(), blocks.end(),
        [&](const BlockInfo &candidate) { return candidate.id == block_id; });
    if (block == blocks.end() ||
        (block->flags & PLAINROOT_STORAGE_BLOCK_REMOVABLE) == 0)
      continue;

    StorageVolume copied;
    copied.id = copy_bytes(volume.id);
    copied.label = copy_bytes(volume.label);
    copied.device_node = block->node;
    copied.filesystem_type = copy_bytes(volume.filesystem_type);
    copied.size_bytes = block->size_bytes;
    copied.read_only =
        (volume.flags & PLAINROOT_STORAGE_VOLUME_READ_ONLY) != 0 ||
        (block->flags & PLAINROOT_STORAGE_BLOCK_READ_ONLY) != 0;
    copied.mounted = (volume.flags & PLAINROOT_STORAGE_VOLUME_MOUNTED) != 0;
    for (const MountInfo &mount : mounts) {
      if (mount.major != block->major || mount.minor != block->minor)
        continue;
      copied.mounted = true;
      copied.mount_id = mount.id;
      copied.mount_target = mount.target;
      break;
    }
    if (!copied.id.empty())
      output.removable_volumes.push_back(std::move(copied));
  }
  return output;
}

struct SharedState {
  plainroot_toolkit_application *application = nullptr;
  plainroot_storage_client *client = nullptr;
  int stop_fd = -1;
  int client_fd = -1;
  std::thread poller;
  std::atomic<bool> stopping{false};
  std::atomic<bool> dispatch_posted{false};
  StorageBridge::CatalogHandler catalog_handler;
  StorageBridge::OperationHandler operation_handler;
  StorageBridge::ErrorHandler error_handler;

  ~SharedState() { stop(); }

  void stop() {
    if (!stopping.exchange(true, std::memory_order_acq_rel)) {
      if (stop_fd >= 0) {
        const uint64_t wake = 1;
        (void)write(stop_fd, &wake, sizeof(wake));
      }
    }
    if (poller.joinable())
      poller.join();
    if (client != nullptr) {
      plainroot_storage_client_destroy_v1(client);
      client = nullptr;
    }
    client_fd = -1;
    if (stop_fd >= 0) {
      close(stop_fd);
      stop_fd = -1;
    }
    catalog_handler = {};
    operation_handler = {};
    error_handler = {};
  }
};

struct DispatchTask {
  std::shared_ptr<SharedState> state;
};

void on_catalog(const plainroot_storage_client_snapshot *snapshot, uint32_t,
                void *user_data) {
  auto &state = *static_cast<SharedState *>(user_data);
  if (!state.catalog_handler || state.stopping.load(std::memory_order_acquire))
    return;
  try {
    state.catalog_handler(copy_catalog(snapshot));
  } catch (...) {
    try {
      if (state.error_handler)
        state.error_handler({PLAINROOT_STORAGE_OUT_OF_MEMORY, ENOMEM});
    } catch (...) {
    }
  }
}

void on_operation(const plainroot_storage_client_operation_event_v1 *event,
                  void *user_data) {
  auto &state = *static_cast<SharedState *>(user_data);
  if (event == nullptr || !state.operation_handler ||
      state.stopping.load(std::memory_order_acquire))
    return;
  const StorageOperationUpdate update{event->type,
                                      event->request_kind,
                                      event->operation_kind,
                                      event->operation_state,
                                      event->operation_error,
                                      event->result};
  try {
    state.operation_handler(update);
  } catch (...) {
    try {
      if (state.error_handler)
        state.error_handler({PLAINROOT_STORAGE_OUT_OF_MEMORY, ENOMEM});
    } catch (...) {
    }
  }
}

void run_dispatch(void *user_data) {
  auto *task = static_cast<DispatchTask *>(user_data);
  const std::shared_ptr<SharedState> state = task->state;
  if (state->stopping.load(std::memory_order_acquire) ||
      state->client == nullptr)
    return;
  size_t dispatched = 0;
  plainroot_storage_error_v1 error{};
  error.struct_size = sizeof(error);
  const auto result = plainroot_storage_client_dispatch_v1(
      state->client, &on_catalog, state.get(), &dispatched, &error);
  state->dispatch_posted.store(false, std::memory_order_release);
  const uint64_t wake = 1;
  if (state->stop_fd >= 0)
    (void)write(state->stop_fd, &wake, sizeof(wake));
  if (result != PLAINROOT_STORAGE_OK && state->error_handler &&
      !state->stopping.load(std::memory_order_acquire)) {
    try {
      state->error_handler({result, error.native_code});
    } catch (...) {
    }
  }
}

void destroy_dispatch(void *user_data) {
  delete static_cast<DispatchTask *>(user_data);
}

bool queue_dispatch(const std::shared_ptr<SharedState> &state) {
  bool expected = false;
  if (!state->dispatch_posted.compare_exchange_strong(
          expected, true, std::memory_order_acq_rel))
    return true;
  auto *task = new (std::nothrow) DispatchTask{state};
  if (task == nullptr) {
    state->dispatch_posted.store(false, std::memory_order_release);
    return false;
  }
  const auto result = plainroot_toolkit_application_post_v1(
      state->application, &run_dispatch, task, &destroy_dispatch);
  if (result != PLAINROOT_TOOLKIT_RESULT_OK) {
    state->dispatch_posted.store(false, std::memory_order_release);
    delete task;
    return false;
  }
  return true;
}

void drain_eventfd(int fd) {
  uint64_t value = 0;
  while (read(fd, &value, sizeof(value)) ==
         static_cast<ssize_t>(sizeof(value))) {
  }
}

void poll_client(const std::shared_ptr<SharedState> &state) {
  pollfd descriptors[2] = {{state->client_fd, POLLIN, 0},
                           {state->stop_fd, POLLIN, 0}};
  for (;;) {
    int result;
    do {
      result = poll(descriptors, 2, -1);
    } while (result < 0 && errno == EINTR);
    if (result < 0 || (descriptors[1].revents & POLLIN) != 0 ||
        state->stopping.load(std::memory_order_acquire))
      return;
    if ((descriptors[0].revents & (POLLIN | POLLERR | POLLHUP)) == 0)
      continue;

    if (!queue_dispatch(state)) {
      pollfd wake{state->stop_fd, POLLIN, 0};
      (void)poll(&wake, 1, 100);
      if ((wake.revents & POLLIN) != 0) {
        drain_eventfd(state->stop_fd);
        if (state->stopping.load(std::memory_order_acquire))
          return;
      }
      continue;
    }

    while (state->dispatch_posted.load(std::memory_order_acquire)) {
      pollfd wake{state->stop_fd, POLLIN, 0};
      int wait_result;
      do {
        wait_result = poll(&wake, 1, -1);
      } while (wait_result < 0 && errno == EINTR);
      if (wait_result < 0)
        return;
      if ((wake.revents & POLLIN) != 0)
        drain_eventfd(state->stop_fd);
      if (state->stopping.load(std::memory_order_acquire))
        return;
    }
  }
}

} // namespace

class StorageBridge::Impl {
public:
  Impl(plainroot_toolkit_application *application,
       CatalogHandler catalog_handler, OperationHandler operation_handler,
       ErrorHandler error_handler)
      : state_(std::make_shared<SharedState>()) {
    state_->application = application;
    state_->catalog_handler = std::move(catalog_handler);
    state_->operation_handler = std::move(operation_handler);
    state_->error_handler = std::move(error_handler);
    if (application == nullptr)
      return;

    plainroot_storage_error_v1 error{};
    error.struct_size = sizeof(error);
    if (plainroot_storage_client_create_v1(nullptr, &state_->client, &error) !=
            PLAINROOT_STORAGE_OK ||
        state_->client == nullptr) {
      if (state_->error_handler)
        state_->error_handler({PLAINROOT_STORAGE_NOT_READY, error.native_code});
      return;
    }
    if (plainroot_storage_client_set_operation_callback_v1(
            state_->client, &on_operation, state_.get(), &error) !=
        PLAINROOT_STORAGE_OK) {
      plainroot_storage_client_destroy_v1(state_->client);
      state_->client = nullptr;
      if (state_->error_handler)
        state_->error_handler({PLAINROOT_STORAGE_IO_ERROR, error.native_code});
      return;
    }
    state_->client_fd =
        plainroot_storage_client_notification_fd_v1(state_->client);
    state_->stop_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (state_->client_fd < 0 || state_->stop_fd < 0) {
      if (state_->client != nullptr) {
        plainroot_storage_client_destroy_v1(state_->client);
        state_->client = nullptr;
      }
      if (state_->stop_fd >= 0) {
        close(state_->stop_fd);
        state_->stop_fd = -1;
      }
      if (state_->error_handler)
        state_->error_handler({PLAINROOT_STORAGE_IO_ERROR, errno});
      return;
    }
    try {
      const auto shared = state_;
      state_->poller = std::thread([shared] { poll_client(shared); });
      ready_ = true;
    } catch (...) {
      state_->stop();
      if (state_->error_handler)
        state_->error_handler({PLAINROOT_STORAGE_OUT_OF_MEMORY, EAGAIN});
    }
  }

  ~Impl() { stop(); }

  bool ready() const { return ready_; }

  void stop() {
    if (state_ != nullptr)
      state_->stop();
    ready_ = false;
  }

  plainroot_storage_result_v1 mount_volume(const std::string &id,
                                           bool read_only) {
    if (!ready_ || state_->client == nullptr)
      return PLAINROOT_STORAGE_NOT_READY;
    uint64_t request_id = 0;
    return plainroot_storage_client_mount_volume_v1(
        state_->client, {id.data(), id.size()}, read_only ? 1 : 0, &request_id,
        nullptr);
  }

  plainroot_storage_result_v1 unmount_mount(const std::string &id) {
    if (!ready_ || state_->client == nullptr)
      return PLAINROOT_STORAGE_NOT_READY;
    uint64_t request_id = 0;
    return plainroot_storage_client_unmount_mount_v1(
        state_->client, {id.data(), id.size()}, &request_id, nullptr);
  }

  plainroot_storage_result_v1 request_snapshot() {
    if (!ready_ || state_->client == nullptr)
      return PLAINROOT_STORAGE_NOT_READY;
    return plainroot_storage_client_request_snapshot_v1(state_->client,
                                                        nullptr);
  }

private:
  std::shared_ptr<SharedState> state_;
  bool ready_ = false;
};

StorageBridge::StorageBridge(plainroot_toolkit_application *application,
                             CatalogHandler catalog_handler,
                             OperationHandler operation_handler,
                             ErrorHandler error_handler)
    : impl_(std::make_unique<Impl>(application, std::move(catalog_handler),
                                   std::move(operation_handler),
                                   std::move(error_handler))) {}

StorageBridge::~StorageBridge() = default;

bool StorageBridge::ready() const { return impl_ && impl_->ready(); }

void StorageBridge::stop() {
  if (impl_)
    impl_->stop();
}

plainroot_storage_result_v1 StorageBridge::mount_volume(const std::string &id,
                                                        bool read_only) {
  return impl_ ? impl_->mount_volume(id, read_only)
               : PLAINROOT_STORAGE_NOT_READY;
}

plainroot_storage_result_v1
StorageBridge::unmount_mount(const std::string &id) {
  return impl_ ? impl_->unmount_mount(id) : PLAINROOT_STORAGE_NOT_READY;
}

plainroot_storage_result_v1 StorageBridge::request_snapshot() {
  return impl_ ? impl_->request_snapshot() : PLAINROOT_STORAGE_NOT_READY;
}

} // namespace plainroot::filechooser::detail
