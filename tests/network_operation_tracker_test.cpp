#include "../src/network_operation_tracker.hpp"

#include <cstdlib>
#include <iostream>

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

  NetworkOperationTracker tracker;
  tracker.begin(17U, PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT);

  CHECK(!tracker.complete_request(
      18U, PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT, PLAINROOT_STORAGE_OK,
      "other-operation"));
  CHECK(tracker.complete_request(
      17U, PLAINROOT_STORAGE_NETWORK_REQUEST_CONNECT, PLAINROOT_STORAGE_OK,
      "chooser-operation"));
  CHECK(!tracker.matches_operation("other-operation"));
  CHECK(tracker.matches_operation("chooser-operation"));

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
