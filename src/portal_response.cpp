#include "portal_response.hpp"

#include "portal_choices.hpp"

#include <cerrno>

namespace plainroot::filechooser {
namespace {

constexpr uint32_t kResponseSuccess = 0U;

bool append_uris(sd_bus_message *reply,
                 const std::vector<std::string> &uris) {
  int result = sd_bus_message_open_container(reply, SD_BUS_TYPE_DICT_ENTRY,
                                              "sv");
  if (result >= 0)
    result = sd_bus_message_append(reply, "s", "uris");
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_VARIANT, "as");
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "s");
  for (const std::string &uri : uris)
    if (result >= 0)
      result = sd_bus_message_append(reply, "s", uri.c_str());
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  return result >= 0;
}

bool append_filter_option(sd_bus_message *reply,
                          const BrowserFilter &filter) {
  int result = sd_bus_message_open_container(reply, SD_BUS_TYPE_DICT_ENTRY,
                                              "sv");
  if (result >= 0)
    result = sd_bus_message_append(reply, "s", "current_filter");
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_VARIANT,
                                            "(sa(us))");
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT,
                                            "sa(us)");
  if (result >= 0)
    result = sd_bus_message_append(reply, "s", filter.name.c_str());
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "(us)");
  for (const BrowserFilterPattern &pattern : filter.patterns) {
    if (result >= 0)
      result = sd_bus_message_open_container(reply, SD_BUS_TYPE_STRUCT, "us");
    if (result >= 0)
      result = sd_bus_message_append(reply, "us", pattern.type,
                                     pattern.pattern.c_str());
    if (result >= 0)
      result = sd_bus_message_close_container(reply);
  }
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  return result >= 0;
}

} // namespace

int send_portal_response(
    sd_bus *bus, sd_bus_message *call, uint32_t response,
    const std::vector<std::string> &uris,
    const BrowserFilter *current_filter,
    const std::vector<BrowserChoiceSelection> *choices) {
  if (bus == nullptr || call == nullptr)
    return -EINVAL;

  sd_bus_message *reply = nullptr;
  int result = sd_bus_message_new_method_return(call, &reply);
  if (result >= 0)
    result = sd_bus_message_append(reply, "u", response);
  if (result >= 0)
    result = sd_bus_message_open_container(reply, SD_BUS_TYPE_ARRAY, "{sv}");
  if (result >= 0 && !uris.empty() && !append_uris(reply, uris))
    result = -EINVAL;
  if (result >= 0 && response == kResponseSuccess &&
      current_filter != nullptr && !append_filter_option(reply, *current_filter))
    result = -EINVAL;
  if (result >= 0 && response == kResponseSuccess && choices != nullptr &&
      !choices->empty() && !append_portal_choices(reply, *choices))
    result = -EINVAL;
  if (result >= 0)
    result = sd_bus_message_close_container(reply);
  if (result >= 0)
    result = sd_bus_send(bus, reply, nullptr);
  sd_bus_message_unref(reply);
  return result;
}

} // namespace plainroot::filechooser
