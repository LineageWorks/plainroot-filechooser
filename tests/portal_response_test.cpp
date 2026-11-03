#include "../src/portal_response.hpp"

#include <systemd/sd-bus.h>

#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <string>
#include <vector>

namespace {

constexpr char kName[] = "org.plainroot.FileChooser.ResponseTest";
constexpr char kPath[] = "/org/freedesktop/portal/desktop";
constexpr char kInterface[] = "org.freedesktop.impl.portal.FileChooser";
constexpr char kRequestHandle[] =
    "/org/freedesktop/portal/desktop/request/test/response";

struct ServerState {
  sd_bus_message *pending_call = nullptr;
  size_t call_count = 0;
};

struct ClientState {
  bool completed = false;
  int error = 0;
  uint32_t response = UINT32_MAX;
  std::vector<std::string> uris;
  bool has_filter = false;
  bool has_choices = false;
};

int open_file(sd_bus_message *message, void *user_data, sd_bus_error *) {
  auto &state = *static_cast<ServerState *>(user_data);
  if (state.pending_call != nullptr)
    return -EBUSY;
  state.pending_call = sd_bus_message_ref(message);
  ++state.call_count;
  return 1;
}

const sd_bus_vtable kVtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("OpenFile", "osssa{sv}", "ua{sv}", open_file,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

int response_received(sd_bus_message *message, void *user_data,
                      sd_bus_error *) {
  auto &state = *static_cast<ClientState *>(user_data);
  if (sd_bus_message_is_method_error(message, nullptr) > 0) {
    state.error = -EIO;
    state.completed = true;
    return 1;
  }
  int result = sd_bus_message_read(message, "u", &state.response);
  if (result < 0 ||
      sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") <= 0) {
    state.error = result < 0 ? result : -EBADMSG;
    state.completed = true;
    return 1;
  }

  while (true) {
    result = sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY,
                                            "sv");
    if (result == 0)
      break;
    if (result < 0) {
      state.error = result;
      break;
    }
    const char *key = nullptr;
    char type = 0;
    const char *contents = nullptr;
    result = sd_bus_message_read(message, "s", &key);
    if (result >= 0)
      result = sd_bus_message_peek_type(message, &type, &contents);
    if (result <= 0 || key == nullptr || type != SD_BUS_TYPE_VARIANT ||
        contents == nullptr ||
        sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT,
                                       contents) <= 0) {
      state.error = result < 0 ? result : -EBADMSG;
      break;
    }

    if (std::strcmp(key, "uris") == 0) {
      if (std::strcmp(contents, "as") != 0 ||
          sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "s") <= 0) {
        state.error = -EBADMSG;
        break;
      }
      while (true) {
        const char *uri = nullptr;
        result = sd_bus_message_read(message, "s", &uri);
        if (result == 0)
          break;
        if (result < 0 || uri == nullptr) {
          state.error = result < 0 ? result : -EBADMSG;
          break;
        }
        state.uris.emplace_back(uri);
      }
      if (state.error == 0 && sd_bus_message_exit_container(message) < 0)
        state.error = -EBADMSG;
    } else {
      if (std::strcmp(key, "current_filter") == 0) {
        state.has_filter = std::strcmp(contents, "(sa(us))") == 0;
      } else if (std::strcmp(key, "choices") == 0) {
        state.has_choices = std::strcmp(contents, "a(ss)") == 0;
      }
      result = sd_bus_message_skip(message, contents);
      if (result < 0)
        state.error = result;
    }

    if (sd_bus_message_exit_container(message) < 0 ||
        sd_bus_message_exit_container(message) < 0) {
      state.error = -EBADMSG;
      break;
    }
    if (state.error != 0)
      break;
  }
  if (state.error == 0 && sd_bus_message_exit_container(message) < 0)
    state.error = -EBADMSG;
  state.completed = true;
  return 1;
}

bool dispatch_once(sd_bus *bus) {
  int result = 0;
  do {
    result = sd_bus_process(bus, nullptr);
  } while (result > 0 || result == -EINTR);
  return result >= 0;
}

template <typename Predicate>
bool dispatch_until(sd_bus *server, sd_bus *client, Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!dispatch_once(server) || !dispatch_once(client))
      return false;
    if (predicate())
      return true;

    pollfd descriptors[2]{};
    descriptors[0].fd = sd_bus_get_fd(server);
    descriptors[0].events = static_cast<short>(sd_bus_get_events(server));
    descriptors[1].fd = sd_bus_get_fd(client);
    descriptors[1].events = static_cast<short>(sd_bus_get_events(client));
    if (descriptors[0].fd < 0 || descriptors[1].fd < 0)
      return false;
    const int result = poll(descriptors, 2, 10);
    if (result < 0 && errno != EINTR)
      return false;
  }
  return false;
}

sd_bus_message *make_open_call(sd_bus *bus) {
  sd_bus_message *message = nullptr;
  assert(sd_bus_message_new_method_call(bus, &message, kName, kPath, kInterface,
                                        "OpenFile") >= 0);
  assert(sd_bus_message_append(message, "osss", kRequestHandle,
                               "org.plainroot.ResponseTest", "", "Open") >=
         0);
  assert(sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}") >=
         0);
  assert(sd_bus_message_close_container(message) >= 0);
  return message;
}

bool begin_request(sd_bus *server, sd_bus *client, ClientState &client_state,
                   ServerState &server_state, sd_bus_slot **reply_slot) {
  sd_bus_message *call = make_open_call(client);
  const int result = sd_bus_call_async(client, reply_slot, call,
                                      response_received, &client_state,
                                      3000000U);
  sd_bus_message_unref(call);
  if (result < 0 || sd_bus_flush(client) < 0)
    return false;
  return dispatch_until(server, client, [&] {
    return server_state.pending_call != nullptr || client_state.completed;
  }) && server_state.pending_call != nullptr && !client_state.completed;
}

bool complete_request(sd_bus *server, sd_bus *client,
                      ClientState &client_state, ServerState &server_state,
                      uint32_t response, const std::vector<std::string> &uris,
                      const plainroot::filechooser::BrowserFilter *filter,
                      const std::vector<plainroot::filechooser::BrowserChoiceSelection>
                          *choices) {
  const int sent = plainroot::filechooser::send_portal_response(
      server, server_state.pending_call, response, uris, filter, choices);
  sd_bus_message_unref(server_state.pending_call);
  server_state.pending_call = nullptr;
  if (sent < 0 || sd_bus_flush(server) < 0)
    return false;
  return dispatch_until(server, client, [&] { return client_state.completed; });
}

} // namespace

int main() {
  sd_bus *server = nullptr;
  sd_bus *client = nullptr;
  sd_bus_slot *service_slot = nullptr;
  sd_bus_slot *reply_slot = nullptr;
  ServerState server_state;
  assert(sd_bus_open_user(&server) >= 0);
  assert(sd_bus_open_user(&client) >= 0);
  assert(sd_bus_request_name(server, kName, 0) >= 0);
  assert(sd_bus_add_object_vtable(server, &service_slot, kPath, kInterface,
                                  kVtable, &server_state) >= 0);

  ClientState accepted;
  assert(begin_request(server, client, accepted, server_state, &reply_slot));
  assert(server_state.call_count == 1U);
  assert(!accepted.completed); // The portal call remains pending until finish.
  const plainroot::filechooser::BrowserFilter filter{
      "Text files", {{0U, "*.txt"}}};
  const std::vector<plainroot::filechooser::BrowserChoiceSelection> choices{
      {"encoding", "utf8"}};
  assert(complete_request(server, client, accepted, server_state, 0U,
                          {"file:///tmp/Plainroot%20notes.txt"}, &filter,
                          &choices));
  assert(accepted.error == 0);
  assert(accepted.response == 0U);
  assert(accepted.uris.size() == 1U);
  assert(accepted.uris.front() == "file:///tmp/Plainroot%20notes.txt");
  assert(accepted.has_filter && accepted.has_choices);
  reply_slot = sd_bus_slot_unref(reply_slot);

  ClientState cancelled;
  assert(begin_request(server, client, cancelled, server_state, &reply_slot));
  assert(server_state.call_count == 2U);
  assert(!cancelled.completed);
  assert(complete_request(server, client, cancelled, server_state, 1U, {},
                          nullptr, nullptr));
  assert(cancelled.error == 0);
  assert(cancelled.response == 1U);
  assert(cancelled.uris.empty());
  assert(!cancelled.has_filter && !cancelled.has_choices);

  reply_slot = sd_bus_slot_unref(reply_slot);
  service_slot = sd_bus_slot_unref(service_slot);
  client = sd_bus_flush_close_unref(client);
  server = sd_bus_flush_close_unref(server);
  return 0;
}
