#include "portal_accept_label.hpp"

#include <glib.h>

namespace plainroot::filechooser::detail {

ParsedAcceptLabel parse_portal_accept_label(std::string_view label) {
  ParsedAcceptLabel parsed;
  parsed.display_label.reserve(label.size());

  if (label.empty())
    return parsed;
  if (label.size() > static_cast<size_t>(G_MAXSSIZE) ||
      !g_utf8_validate(label.data(), static_cast<gssize>(label.size()),
                       nullptr)) {
    parsed.display_label.assign(label.data(), label.size());
    return parsed;
  }

  const gchar *current = label.data();
  const gchar *end = current + label.size();
  while (current < end) {
    if (*current == '_') {
      const gchar *next = current + 1;
      if (next < end && *next == '_') {
        parsed.display_label.push_back('_');
        current = next + 1;
        continue;
      }
      if (next < end) {
        const gchar *character_end = g_utf8_next_char(next);
        const gunichar character = g_utf8_get_char(next);
        if (parsed.mnemonic_utf8.empty()) {
          gchar encoded[8]{};
          const gint encoded_size = g_unichar_to_utf8(
              g_unichar_tolower(character), encoded);
          if (encoded_size > 0)
            parsed.mnemonic_utf8.assign(encoded,
                                        static_cast<size_t>(encoded_size));
        }
        parsed.display_label.append(next,
                                    static_cast<size_t>(character_end - next));
        current = character_end;
        continue;
      }
    }

    const gchar *character_end = g_utf8_next_char(current);
    parsed.display_label.append(
        current, static_cast<size_t>(character_end - current));
    current = character_end;
  }
  return parsed;
}

} // namespace plainroot::filechooser::detail
