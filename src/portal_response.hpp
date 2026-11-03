#pragma once

#include "browser_choice.hpp"
#include "browser_filter.hpp"

#include <systemd/sd-bus.h>

#include <cstdint>
#include <string>
#include <vector>

namespace plainroot::filechooser {

// Sends the delayed FileChooser method reply. The return value is the sd-bus
// result (negative errno on failure, otherwise the sent message cookie).
int send_portal_response(
    sd_bus *bus, sd_bus_message *call, uint32_t response,
    const std::vector<std::string> &uris,
    const BrowserFilter *current_filter,
    const std::vector<BrowserChoiceSelection> *choices,
    bool writable);

} // namespace plainroot::filechooser
