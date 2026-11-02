#include "../src/portal_choices.hpp"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

using plainroot::filechooser::BrowserChoice;
using plainroot::filechooser::BrowserChoiceOption;
using plainroot::filechooser::BrowserChoiceSelection;
using plainroot::filechooser::append_portal_choices;
using plainroot::filechooser::parse_portal_choices;

namespace {

sd_bus *test_bus = nullptr;

sd_bus_message *make_message() {
  sd_bus_message *message = nullptr;
  if (test_bus == nullptr)
    assert(sd_bus_open_user(&test_bus) >= 0);
  assert(sd_bus_message_new_method_call(
             test_bus, &message, "org.plainroot.FileChooser.Test",
             "/org/plainroot/FileChooser/Test",
             "org.plainroot.FileChooser.Test", "Test") >= 0);
  return message;
}

bool append_choice(sd_bus_message *message, const BrowserChoice &choice) {
  if (sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT,
                                    "ssa(ss)s") < 0 ||
      sd_bus_message_append(message, "ss", choice.id.c_str(),
                            choice.label.c_str()) < 0 ||
      sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "(ss)") < 0)
    return false;
  for (const BrowserChoiceOption &option : choice.options) {
    if (sd_bus_message_open_container(message, SD_BUS_TYPE_STRUCT, "ss") < 0 ||
        sd_bus_message_append(message, "ss", option.id.c_str(),
                              option.label.c_str()) < 0 ||
        sd_bus_message_close_container(message) < 0)
      return false;
  }
  return sd_bus_message_close_container(message) >= 0 &&
         sd_bus_message_append(message, "s", choice.initial_selection.c_str()) >=
             0 &&
         sd_bus_message_close_container(message) >= 0;
}

sd_bus_message *make_choice_array(const std::vector<BrowserChoice> &choices) {
  sd_bus_message *message = make_message();
  assert(sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY,
                                       "(ssa(ss)s)") >= 0);
  for (const BrowserChoice &choice : choices)
    assert(append_choice(message, choice));
  assert(sd_bus_message_close_container(message) >= 0);
  assert(sd_bus_message_seal(message, 1U, 0U) >= 0);
  assert(sd_bus_message_rewind(message, true) >= 0);
  return message;
}

std::vector<BrowserChoice> example_choices() {
  return {
      {"encoding", "Encoding",
       {{"utf8", "Unicode (UTF-8)"}, {"latin15", "Western"}}, "latin15"},
      {"reencode", "Reencode", {}, "false"},
  };
}

} // namespace

int main() {
  const auto expected = example_choices();
  sd_bus_message *message = make_choice_array(expected);
  std::vector<BrowserChoice> parsed;
  assert(parse_portal_choices(message, parsed));
  assert(parsed.size() == expected.size());
  assert(parsed[0].id == "encoding");
  assert(parsed[0].label == "Encoding");
  assert(parsed[0].options.size() == 2U);
  assert(parsed[0].options[1].id == "latin15");
  assert(parsed[0].initial_selection == "latin15");
  assert(parsed[1].options.empty());
  assert(parsed[1].initial_selection == "false");
  sd_bus_message_unref(message);

  message = make_choice_array({{"duplicate", "First", {}, ""},
                               {"duplicate", "Second", {}, ""}});
  parsed.clear();
  assert(!parse_portal_choices(message, parsed));
  sd_bus_message_unref(message);

  message = make_choice_array({{"encoding", "Encoding",
                                {{"utf8", "Unicode (UTF-8)"}}, "missing"}});
  parsed.clear();
  assert(!parse_portal_choices(message, parsed));
  sd_bus_message_unref(message);

  message = make_message();
  const std::vector<BrowserChoiceSelection> selected{
      {"encoding", "utf8"}, {"reencode", "true"}};
  assert(sd_bus_message_open_container(message, SD_BUS_TYPE_ARRAY, "{sv}") >=
         0);
  assert(append_portal_choices(message, selected));
  assert(sd_bus_message_close_container(message) >= 0);
  assert(sd_bus_message_seal(message, 2U, 0U) >= 0);
  assert(sd_bus_message_rewind(message, true) >= 0);
  assert(sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "{sv}") >
         0);
  assert(sd_bus_message_enter_container(message, SD_BUS_TYPE_DICT_ENTRY,
                                        "sv") > 0);
  const char *key = nullptr;
  assert(sd_bus_message_read(message, "s", &key) >= 0);
  assert(key != nullptr && std::strcmp(key, "choices") == 0);
  assert(sd_bus_message_enter_container(message, SD_BUS_TYPE_VARIANT,
                                        "a(ss)") > 0);
  assert(sd_bus_message_enter_container(message, SD_BUS_TYPE_ARRAY, "(ss)") >
         0);
  for (const BrowserChoiceSelection &expected_choice : selected) {
    assert(sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "ss") >
           0);
    const char *id = nullptr;
    const char *value = nullptr;
    assert(sd_bus_message_read(message, "ss", &id, &value) >= 0);
    assert(id != nullptr && expected_choice.id == id);
    assert(value != nullptr && expected_choice.value == value);
    assert(sd_bus_message_exit_container(message) >= 0);
  }
  assert(sd_bus_message_enter_container(message, SD_BUS_TYPE_STRUCT, "ss") ==
         0);
  assert(sd_bus_message_exit_container(message) >= 0);
  assert(sd_bus_message_exit_container(message) >= 0);
  assert(sd_bus_message_exit_container(message) >= 0);
  assert(sd_bus_message_exit_container(message) >= 0);
  sd_bus_message_unref(message);
  test_bus = sd_bus_unref(test_bus);
  return 0;
}
