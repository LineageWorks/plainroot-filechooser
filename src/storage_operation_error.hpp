#pragma once

#include <plainroot/storage/client.h>

#include <cstdint>

namespace plainroot::filechooser::detail {

inline const char *storage_operation_error_message(uint32_t error) noexcept {
  switch (error) {
  case PLAINROOT_STORAGE_OPERATION_ERROR_AUTHORIZATION_DENIED:
    return "Storage authorization was denied.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_INCOMPLETE_SNAPSHOT:
    return "Storage information is incomplete; refresh and try again.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_STALE_ENTITY:
    return "This storage device changed. Refresh and try again.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_UNSUPPORTED_FILESYSTEM:
    return "This filesystem is not supported for mounting.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_ALREADY_MOUNTED:
    return "This volume is already mounted.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_MOUNT_NOT_OWNED:
    return "This location is managed by another service.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_BUSY:
    return "The storage location is busy. Close files using it and try again.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_MOUNTPOINT_UNAVAILABLE:
    return "A mount location is unavailable.";
  case PLAINROOT_STORAGE_OPERATION_ERROR_AUTHORIZATION_UNAVAILABLE:
    return "Storage authorization is unavailable.";
  default:
    return "The storage operation failed.";
  }
}

} // namespace plainroot::filechooser::detail
