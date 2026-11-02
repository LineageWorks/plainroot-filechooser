#include "ui_context_factory.hpp"

#include <plainroot/i18n/i18n.h>
#include <plainroot/toolkit/toolkit.h>

namespace plainroot::filechooser {

plainroot_ui_context *make_ui_context(const char *language_tag) {
  plainroot_ui_theme_v3 theme{};
  theme.struct_size = sizeof(theme);
  if (plainroot_ui_theme_default_v3(PLAINROOT_UI_THEME_DARK, &theme) !=
      PLAINROOT_UI_RESULT_OK)
    return nullptr;

  plainroot_ui_context_options_v1 options_v1{};
  options_v1.struct_size = sizeof(options_v1);
  options_v1.text_direction =
      plainroot_i18n_get_layout_direction_for_tag_v1(language_tag) ==
              PLAINROOT_I18N_LAYOUT_DIRECTION_RTL
          ? PLAINROOT_UI_TEXT_RTL
          : PLAINROOT_UI_TEXT_LTR;
  options_v1.language = language_tag;
  plainroot_ui_context_options_v2 options_v2{};
  options_v2.struct_size = sizeof(options_v2);
  options_v2.base = options_v1;
  plainroot_ui_context_options_v3 options_v3{};
  options_v3.struct_size = sizeof(options_v3);
  options_v3.base = options_v2;
  options_v3.theme = &theme.base;
  plainroot_ui_context_options_v4 options_v4{};
  options_v4.struct_size = sizeof(options_v4);
  options_v4.base = options_v3;
  options_v4.theme = &theme;

  plainroot_toolkit_font_resolver_options_v1 font_options{};
  font_options.struct_size = sizeof(font_options);
  font_options.language = language_tag;
  auto *fonts = plainroot_toolkit_font_set_create_v1(&font_options);
  if (fonts == nullptr || plainroot_toolkit_font_set_face_count(fonts) == 0) {
    plainroot_toolkit_font_set_destroy(fonts);
    return nullptr;
  }
  plainroot_ui_context_options_v5 options_v5{};
  options_v5.struct_size = sizeof(options_v5);
  options_v5.base = options_v4;
  auto *context =
      plainroot_toolkit_ui_context_create_with_fonts_v1(fonts, &options_v5);
  plainroot_toolkit_font_set_destroy(fonts);
  return context;
}

} // namespace plainroot::filechooser
