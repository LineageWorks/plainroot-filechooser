#pragma once

#include "browser_choice.hpp"

#include <systemd/sd-bus.h>

namespace plainroot::filechooser {

// The message cursor must be at the choices array (inside its variant).
bool parse_portal_choices(sd_bus_message *message,
                          std::vector<BrowserChoice> &choices);

// The message cursor must be inside the response vardict.
bool append_portal_choices(
    sd_bus_message *message,
    const std::vector<BrowserChoiceSelection> &choices);

} // namespace plainroot::filechooser
