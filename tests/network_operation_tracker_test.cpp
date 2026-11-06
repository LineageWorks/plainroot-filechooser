#include "../src/network_operation_error.hpp"
#include "../src/network_operation_tracker.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": "  \
                << #condition << '\n';                                        \
      std::exit(EXIT_FAILURE);                                                 \
    }                                                                          \
  } while (false)

} // namespace

int main() {
  using namespace plainroot::filechooser::detail;

  CHECK(std::string_view(network_operation_error_message(
            PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_BUSY)) ==
        "Close files using this network location and try again.");
  CHECK(std::string_view(network_operation_error_message(UINT32_MAX)) ==
        "The network operation failed.");

  NetworkOperationTracker tracker;
  tracker.begin(17U, PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT);

  CHECK(!tracker.complete_request(
      18U, PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT, PLAINROOT_STORAGE_OK,
      "other-operation"));
  tracker.observe_early_operation(
      "unrelated-operation", PLAINROOT_STORAGE_NETWORK_OPERATION_CONNECT,
      PLAINROOT_STORAGE_NETWORK_OPERATION_FAILED,
      PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_CONNECTION_FAILED);
  tracker.observe_early_operation(
      "chooser-operation", PLAINROOT_STORAGE_NETWORK_OPERATION_CONNECT,
      PLAINROOT_STORAGE_NETWORK_OPERATION_CONNECTING,
      PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_NONE);
  tracker.observe_early_operation(
      "chooser-operation", PLAINROOT_STORAGE_NETWORK_OPERATION_CONNECT,
      PLAINROOT_STORAGE_NETWORK_OPERATION_FAILED,
      PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_MOUNT_FAILED);
  CHECK(tracker.complete_request(
      17U, PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT, PLAINROOT_STORAGE_OK,
      "chooser-operation"));
  CHECK(!tracker.matches_operation("other-operation"));
  CHECK(tracker.matches_operation("chooser-operation"));
  const auto early = tracker.take_matching_early_operations();
  CHECK(early.size() == 2U);
  CHECK(early[0].state == PLAINROOT_STORAGE_NETWORK_OPERATION_CONNECTING);
  CHECK(early[1].state == PLAINROOT_STORAGE_NETWORK_OPERATION_FAILED);
  CHECK(early[1].error ==
        PLAINROOT_STORAGE_NETWORK_OPERATION_ERROR_MOUNT_FAILED);

  tracker.begin(21U, PLAINROOT_STORAGE_NETWORK_REQUEST_DISCONNECT);
  CHECK(tracker.complete_request(
      21U, PLAINROOT_STORAGE_NETWORK_REQUEST_DISCONNECT,
      PLAINROOT_STORAGE_IO_ERROR, ""));
  CHECK(!tracker.matches_operation("chooser-operation"));

  tracker.begin(22U, PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT);
  CHECK(!tracker.complete_request(
      22U, PLAINROOT_STORAGE_NETWORK_REQUEST_DISCONNECT,
      PLAINROOT_STORAGE_OK, "wrong-kind"));

  std::cout << "network operation tracker tests passed\n";
  return EXIT_SUCCESS;
}
