#pragma once

#include <plainroot/storage/network_client.h>

#include <cstdint>

namespace plainroot::filechooser::detail {

inline const char *network_operation_error_message(uint32_t error) {
  switch (error) {
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_INVALID_REQUEST:
    return "Check the server address and try again.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_DEPENDENCY_UNAVAILABLE:
    return "SFTP support is unavailable.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_MOUNTPOINT_UNAVAILABLE:
    return "A mount location is unavailable.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_SYSTEMD_FAILED:
    return "Unable to start the network mount service.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_CONNECTION_FAILED:
    return "Could not connect to the SFTP server.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_MOUNT_FAILED:
    return "The network location could not be mounted. Check the server, host "
           "key, login, and FUSE availability.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_UNMOUNT_FAILED:
    return "Could not disconnect the network location.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_BUSY:
    return "Close files using this network location and try again.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_CANCELLED:
    return "Network connection was cancelled.";
  case PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_OUTCOME_UNKNOWN:
    return "Network operation outcome is uncertain.";
  default:
    return "The network operation failed.";
  }
}

} // namespace plainroot::filechooser::detail
