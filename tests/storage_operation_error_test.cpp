#include "../src/storage_operation_error.hpp"

#include <cstring>
#include <iostream>
#include <limits>

namespace {

bool require(bool condition, const char *message) {
  if (condition)
    return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

} // namespace

int main() {
  using plainroot::filechooser::detail::storage_operation_error_message;

  if (!require(
          std::strcmp(storage_operation_error_message(
                          PLAINROOT_STORAGE_OPERATION_ERROR_BUSY),
                      "The storage location is busy. Close files using it and "
                      "try again.") == 0,
          "busy unmount keeps its specific actionable message") ||
      !require(std::strcmp(storage_operation_error_message(
                               std::numeric_limits<uint32_t>::max()),
                           "The storage operation failed.") == 0,
               "unknown operation errors use the generic fallback")) {
    return 1;
  }
  return 0;
}
