#include "portal_choices.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <utility>

namespace plainroot::filechooser {
namespace {

constexpr size_t kMaximumChoiceGroups = 32U;
constexpr size_t kMaximumOptionsPerChoice = 128U;
constexpr size_t kMaximumTotalOptions = 256U;
constexpr size_t kMaximumChoiceStringLength = 4096U;

bool valid_string(const char *value, bool allow_empty = false) {
  return value != nullptr && (allow_empty || value[0] != '\0') &&
         std::strlen(value) <= kMaximumChoiceStringLength;
}

bool validate_choice(const BrowserChoice &choice,
                     std::unordered_set<std::string> &choice_ids,
                     size_t &total_options) {
  if (!valid_string(choice.id.c_str()) ||
      !valid_string(choice.label.c_str()) ||
      !valid_string(choice.initial_selection.c_str(), true) ||
      !choice_ids.insert(choice.id).second ||
      choice.options.size() > kMaximumOptionsPerChoice ||
      choice.options.size() > kMaximumTotalOptions - total_options)
    return false;

  total_options += choice.options.size();
  std::unordered_set<std::string> option_ids;
  for (const BrowserChoiceOption &option : choice.options) {
    if (!valid_string(option.id.c_str()) ||
        !valid_string(option.label.c_str()) ||
        !option_ids.insert(option.id).second)
      return false;
  }

  if (choice.options.empty())
    return choice.initial_selection.empty() ||
           choice.initial_selection == "true" ||
           choice.initial_selection == "false";

  if (choice.initial_selection.empty())
    return true;
  return std::any_of(choice.options.begin(), choice.options.end(),
                     [&choice](const BrowserChoiceOption &option) {
                       return option.id == choice.initial_selection;
                     });
}

bool validate_choices(const std::vector<BrowserChoice> &choices) {
  if (choices.size() > kMaximumChoiceGroups)
    return false;
  std::unordered_set<std::string> choice_ids;
  size_t total_options = 0;
  for (const BrowserChoice &choice : choices)
    if (!validate_choice(choice, choice_ids, total_options))
      return false;
  return true;
}

} // namespace

bool parse_portal_choices(sd_bus_message *message,
                          std::vector<BrowserChoice> &choices) {
  if (message == nullptr)
    return false;

  std::vector<BrowserChoice> parsed;
  int result = sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY,
                                               "(ssa(ss)s)");
  if (result < 0)
    return false;

  size_t total_options = 0;
  std::unordered_set<std::string> choice_ids;
  while (true) {
    char type = 0;
    const char *contents = nullptr;
    result = sd_bus_message_peek_type(message, &type, &contents);
    if (result == 0)
      break;
    if (result < 0 || type != SD_BUS_TYPE_STRUCT || contents == nullptr ||
        std::strcmp(contents, "ssa(ss)s") != 0 ||
        parsed.size() >= kMaximumChoiceGroups)
      return false;

    BrowserChoice choice;
    result = sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT,
                                             "ssa(ss)s");
    const char *id = nullptr;
    const char *label = nullptr;
    if (result <= 0 ||
        sd_bus_message_read(message, "ss", &id, &label) < 0 ||
        !valid_string(id) || !valid_string(label) ||
        sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(ss)") < 0)
      return false;
    choice.id = id;
    choice.label = label;
    if (!choice_ids.insert(choice.id).second)
      return false;

    while (true) {
      result = sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT,
                                               "ss");
      if (result == 0)
        break;
      if (result < 0 || total_options >= kMaximumTotalOptions ||
          choice.options.size() >= kMaximumOptionsPerChoice)
        return false;
      const char *option_id = nullptr;
      const char *option_label = nullptr;
      if (sd_bus_message_read(message, "ss", &option_id, &option_label) < 0 ||
          !valid_string(option_id) || !valid_string(option_label) ||
          sd_bus_message_exit_container(message) < 0)
        return false;
      choice.options.push_back({option_id, option_label});
      ++total_options;
    }
    if (sd_bus_message_exit_container(message) < 0 ||
        sd_bus_message_read(message, "s", &id) < 0 || !valid_string(id, true) ||
        sd_bus_message_exit_container(message) < 0)
      return false;
    choice.initial_selection = id;
    parsed.push_back(std::move(choice));
  }

  if (sd_bus_message_exit_container(message) < 0 ||
      !validate_choices(parsed))
    return false;
  choices = std::move(parsed);
  return true;
}

bool append_portal_choices(
    sd_bus_message *message,
    const std::vector<BrowserChoiceSelection> &choices) {
  if (message == nullptr)
    return false;

  int result = sd_bus_message_open_container(message, SD_BUS_TYPE_DICT_ENTRY,
                                              "sv");
  if (result >= 0)
    result = sd_bus_message_append(message, "s", "choices");
  if (result >= 0)
    result = sd_bus_message_open_container(message, SD_BUS_TYPE_VARIANT,
                                            "a(ss)");
  if (result >= 0)
    result = sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(ss)");
  for (const BrowserChoiceSelection &choice : choices) {
    if (result >= 0)
      result = sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "ss");
    if (result >= 0)
      result = sd_bus_message_append(message, "ss", choice.id.c_str(),
                                     choice.value.c_str());
    if (result >= 0)
      result = sd_bus_message_close_container(message);
  }
  if (result >= 0)
    result = sd_bus_message_close_container(message);
  if (result >= 0)
    result = sd_bus_message_close_container(message);
  if (result >= 0)
    result = sd_bus_message_close_container(message);
  return result >= 0;
}

} // namespace plainroot::filechooser
