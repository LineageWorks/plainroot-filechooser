#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace plainroot::filechooser::detail {

struct ImagePreview {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

bool make_image_preview(uint32_t source_width, uint32_t source_height,
                        std::span<const uint8_t> source_rgba,
                        uint32_t maximum_width, uint32_t maximum_height,
                        ImagePreview* output);

}  // namespace plainroot::filechooser::detail
