#pragma once

#include <plainroot/storage/network_client.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace plainroot::filechooser::detail {

struct EarlyNetworkOperation {
  std::string operation_id;
  uint32_t operation_kind = PLAINROOT_STORAGE_NETWORK_OPERATION_CONNECT;
  uint32_t state = PLAINROOT_STORAGE_NETWORK_OPERATION_ACCEPTED;
  uint32_t error = PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_NONE;
};

class NetworkOperationTracker {
public:
  void begin(uint64_t request_id, uint32_t request_kind) {
    request_id_ = request_id;
    request_kind_ = request_kind;
    operation_id_.clear();
    early_operations_.clear();
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

  bool awaiting_operation_id() const {
    return request_id_ != 0 && operation_id_.empty();
  }

  void observe_early_operation(std::string_view operation_id,
                               uint32_t operation_kind, uint32_t state,
                               uint32_t error) {
    constexpr std::size_t kMaxEarlyOperations = 16U;
    if (!awaiting_operation_id() || operation_id.empty())
      return;
    if (early_operations_.size() == kMaxEarlyOperations)
      early_operations_.erase(early_operations_.begin());
    early_operations_.push_back(
        {std::string(operation_id), operation_kind, state, error});
  }

  std::vector<EarlyNetworkOperation> take_matching_early_operations() {
    std::vector<EarlyNetworkOperation> matching;
    if (!operation_id_.empty()) {
      for (auto &event : early_operations_) {
        if (event.operation_id == operation_id_)
          matching.push_back(std::move(event));
      }
    }
    early_operations_.clear();
    return matching;
  }

  void reset() {
    request_id_ = 0;
    request_kind_ = 0;
    operation_id_.clear();
    early_operations_.clear();
  }

private:
  uint64_t request_id_ = 0;
  uint32_t request_kind_ = 0;
  std::string operation_id_;
  std::vector<EarlyNetworkOperation> early_operations_;
};

} // namespace plainroot::filechooser::detail
