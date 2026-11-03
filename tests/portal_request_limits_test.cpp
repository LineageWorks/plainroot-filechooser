#include <systemd/sd-bus.h>

#include <cassert>
#include <chrono>
#include <csignal>
#include <cstring>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr char kBackendName[] =
    "org.freedesktop.impl.portal.desktop.plainroot";
constexpr char kDesktopPath[] = "/org/freedesktop/portal/desktop";
constexpr char kChooserInterface[] =
    "org.freedesktop.impl.portal.FileChooser";
constexpr char kRequestPrefix[] =
    "/org/freedesktop/portal/desktop/request/test/limits";
constexpr char kInvalidArgs[] = "org.freedesktop.DBus.Error.InvalidArgs";

bool append_string_option(sd_bus_message *message, const char *key,
                          const std::string &value) {
  return sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY,
                                       "sv") >= 0 &&
         sd_bus_message_append(message, "s", key) >= 0 &&
         sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "s") >=
             0 &&
         sd_bus_message_append(message, "s", value.c_str()) >= 0 &&
         sd_bus_message_close_container(message) >= 0 &&
         sd_bus_message_close_container(message) >= 0;
}

bool append_folder_option(sd_bus_message *message, size_t size) {
  std::vector<char> bytes(size, 'p');
  bytes[0] = '/';
  bytes.back() = '\0';
  return sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY,
                                       "sv") >= 0 &&
         sd_bus_message_append(message, "s", "current_folder") >= 0 &&
         sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT, "ay") >=
             0 &&
         sd_bus_message_append_array(message, SD_BUS_TYPE_BYTE, bytes.data(),
                                     bytes.size()) >= 0 &&
         sd_bus_message_close_container(message) >= 0 &&
         sd_bus_message_close_container(message) >= 0;
}

bool append_filters_option(sd_bus_message *message, size_t filter_count,
                           size_t patterns_per_filter,
                           size_t filter_name_size) {
  const std::string filter_name(filter_name_size, 'n');
  if (sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY, "sv") <
          0 ||
      sd_bus_message_append(message, "s", "filters") < 0 ||
      sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT,
                                    "a(sa(us))") < 0 ||
      sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(sa(us))") <
          0)
    return false;

  for (size_t filter_index = 0; filter_index < filter_count; ++filter_index) {
    if (sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT,
                                      "sa(us)") < 0 ||
        sd_bus_message_append(message, "s", filter_name.c_str()) < 0 ||
        sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(us)") < 0)
      return false;
    for (size_t pattern_index = 0; pattern_index < patterns_per_filter;
         ++pattern_index) {
      if (sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "us") <
              0 ||
          sd_bus_message_append(message, "us", 0U, "*.txt") < 0 ||
          sd_bus_message_close_container(message) < 0)
        return false;
    }
    if (sd_bus_message_close_container(message) < 0 ||
        sd_bus_message_close_container(message) < 0)
      return false;
  }
  return sd_bus_message_close_container(message) >= 0 &&
         sd_bus_message_close_container(message) >= 0 &&
         sd_bus_message_close_container(message) >= 0;
}

bool open_options(sd_bus_message *message) {
  return sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}") >=
         0;
}

bool send_and_expect_invalid(sd_bus *bus, unsigned int sequence,
                             const std::string &title,
                             const std::string &option_key,
                             size_t option_size) {
  sd_bus_message *call = nullptr;
  sd_bus_message *reply = nullptr;
  sd_bus_error error = SD_BUS_ERROR_NULL;
  const std::string handle =
      std::string(kRequestPrefix) + std::to_string(sequence);
  bool valid = sd_bus_message_new_method_call(
                   bus, &call, kBackendName, kDesktopPath, kChooserInterface,
                   "OpenFile") >= 0 &&
               sd_bus_message_append(call, "osss", handle.c_str(),
                                     "org.plainroot.FileChooser.LimitsTest",
                                     "", title.c_str()) >= 0 &&
               open_options(call);
  if (valid && option_key == "accept_label") {
    valid = append_string_option(
        call, "accept_label", std::string(option_size, 'a'));
  } else if (valid && option_key == "current_name") {
    valid = append_string_option(
        call, "current_name", std::string(option_size, 'n'));
  } else if (valid && option_key == "current_folder") {
    valid = append_folder_option(call, option_size);
  } else if (valid && option_key == "filter_name") {
    valid = append_filters_option(call, 1U, 1U, option_size);
  } else if (valid && option_key == "pattern_count") {
    valid = append_filters_option(call, 5U, 256U, 8U);
  }
  valid = valid && sd_bus_message_close_container(call) >= 0;
  if (!valid) {
    sd_bus_message_unref(call);
    return false;
  }

  const int result = sd_bus_call(bus, call, 3'000'000U, &error, &reply);
  const bool rejected = result < 0 && error.name != nullptr &&
                        std::strcmp(error.name, kInvalidArgs) == 0;
  sd_bus_message_unref(reply);
  sd_bus_error_free(&error);
  sd_bus_message_unref(call);
  return rejected;
}

} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  const pid_t backend = fork();
  assert(backend >= 0);
  if (backend == 0) {
    execl(argv[1], argv[1], static_cast<char *>(nullptr));
    _exit(127);
  }

  sd_bus *bus = nullptr;
  if (sd_bus_open_user(&bus) < 0) {
    kill(backend, SIGTERM);
    (void)waitpid(backend, nullptr, 0);
    return 1;
  }

  bool ready = false;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = nullptr;
    int owned = 0;
    const int result = sd_bus_call_method(
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", &error, &reply, "s",
        kBackendName);
    if (result >= 0 && sd_bus_message_read(reply, "b", &owned) >= 0 && owned)
      ready = true;
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (ready)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  bool passed = ready;
  unsigned int sequence = 0U;
  if (passed)
    passed = send_and_expect_invalid(
        bus, sequence++, std::string(4097U, 't'), "", 0U);
  if (passed)
    passed = send_and_expect_invalid(bus, sequence++, "Open", "accept_label",
                                     4097U);
  if (passed)
    passed = send_and_expect_invalid(bus, sequence++, "Open", "current_name",
                                     4097U);
  if (passed)
    passed = send_and_expect_invalid(bus, sequence++, "Open", "current_folder",
                                     65537U);
  if (passed)
    passed = send_and_expect_invalid(bus, sequence++, "Open", "filter_name",
                                     4097U);
  if (passed)
    passed = send_and_expect_invalid(bus, sequence++, "Open", "pattern_count",
                                     0U);
  kill(backend, SIGTERM);
  (void)waitpid(backend, nullptr, 0);
  sd_bus_unref(bus);
  assert(passed);
  return 0;
}
