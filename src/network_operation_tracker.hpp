#pragma once

#include <plainroot/storage/network_client.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace plainroot::filechooser::detail {

class NetworkOperationTracker {
public:
  void begin(uint64_t request_id, uint32_t request_kind) {
    request_id_ = request_id;
    request_kind_ = request_kind;
    operation_id_.clear();
  }

  // Returns true only for the request this chooser started. A successful
  // completion binds subsequent broadcast operation events to its operation.
  bool complete_request(uint64_t request_id, uint32_t request_kind,
                        plainroot_storage_result_v1 result,
                        std::string_view operation_id) {
    if (request_id == 0 || request_id != request_id_ ||
        request_kind != request_kind_)
      return false;

    if (result == PLAINROOT_STORAGE_OK && !operation_id.empty()) {
      operation_id_.assign(operation_id);
    } else {
      reset();
    }
    return true;
  }

  bool matches_operation(std::string_view operation_id) const {
    return !operation_id_.empty() && operation_id == operation_id_;
  }

  void reset() {
    request_id_ = 0;
    request_kind_ = 0;
    operation_id_.clear();
  }

private:
  uint64_t request_id_ = 0;
  uint32_t request_kind_ = 0;
  std::string operation_id_;
};

} // namespace plainroot::filechooser::detail
