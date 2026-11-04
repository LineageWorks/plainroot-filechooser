#include "image_preview.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace plainroot::filechooser::detail {
namespace {

bool checked_byte_count(uint32_t width, uint32_t height, size_t* count) {
  if (width == 0 || height == 0 || count == nullptr)
    return false;
  const uint64_t bytes = static_cast<uint64_t>(width) * height * 4U;
  if (bytes > std::numeric_limits<size_t>::max())
    return false;
  *count = static_cast<size_t>(bytes);
  return true;
}

uint8_t interpolate(uint8_t first, uint8_t second, float amount) {
  return static_cast<uint8_t>(std::clamp(
      std::lround(static_cast<float>(first) * (1.0F - amount) +
                  static_cast<float>(second) * amount),
      0L, 255L));
}

}  // namespace

bool make_image_preview(uint32_t source_width, uint32_t source_height,
                        std::span<const uint8_t> source_rgba,
                        uint32_t maximum_width, uint32_t maximum_height,
                        ImagePreview* output) {
  if (output == nullptr || maximum_width == 0 || maximum_height == 0)
    return false;
  size_t source_bytes = 0;
  if (!checked_byte_count(source_width, source_height, &source_bytes) ||
      source_rgba.size() < source_bytes)
    return false;

  const double fit = std::min(
      {1.0, static_cast<double>(maximum_width) / source_width,
       static_cast<double>(maximum_height) / source_height});
  const auto width = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::lround(source_width * fit)));
  const auto height = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::lround(source_height * fit)));
  size_t output_bytes = 0;
  if (!checked_byte_count(width, height, &output_bytes))
    return false;

  try {
    ImagePreview next{};
    next.width = width;
    next.height = height;
    next.rgba.resize(output_bytes);
    for (uint32_t y = 0; y < height; ++y) {
      const float source_y =
          (static_cast<float>(y) + 0.5F) * source_height / height - 0.5F;
      const float clamped_y = std::clamp(source_y, 0.0F,
                                         static_cast<float>(source_height - 1));
      const uint32_t y0 = static_cast<uint32_t>(std::floor(clamped_y));
      const uint32_t y1 = std::min(y0 + 1, source_height - 1);
      const float fy = clamped_y - y0;
      for (uint32_t x = 0; x < width; ++x) {
        const float source_x =
            (static_cast<float>(x) + 0.5F) * source_width / width - 0.5F;
        const float clamped_x = std::clamp(
            source_x, 0.0F, static_cast<float>(source_width - 1));
        const uint32_t x0 = static_cast<uint32_t>(std::floor(clamped_x));
        const uint32_t x1 = std::min(x0 + 1, source_width - 1);
        const float fx = clamped_x - x0;
        const size_t output_offset =
            (static_cast<size_t>(y) * width + x) * 4U;
        const size_t top_left =
            (static_cast<size_t>(y0) * source_width + x0) * 4U;
        const size_t top_right =
            (static_cast<size_t>(y0) * source_width + x1) * 4U;
        const size_t bottom_left =
            (static_cast<size_t>(y1) * source_width + x0) * 4U;
        const size_t bottom_right =
            (static_cast<size_t>(y1) * source_width + x1) * 4U;
        for (size_t channel = 0; channel < 4; ++channel) {
          const uint8_t top = interpolate(source_rgba[top_left + channel],
                                          source_rgba[top_right + channel], fx);
          const uint8_t bottom = interpolate(
              source_rgba[bottom_left + channel],
              source_rgba[bottom_right + channel], fx);
          next.rgba[output_offset + channel] = interpolate(top, bottom, fy);
        }
      }
    }
    *output = std::move(next);
    return true;
  } catch (const std::bad_alloc&) {
    return false;
  }
}

}  // namespace plainroot::filechooser::detail
