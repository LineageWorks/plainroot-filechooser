#include "../src/image_preview.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <span>

namespace {

bool require(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool bounds_and_aspect_ratio() {
  std::array<uint8_t, 12U * 6U * 4U> pixels{};
  plainroot::filechooser::detail::ImagePreview preview;
  if (!require(plainroot::filechooser::detail::make_image_preview(
                   12, 6, pixels, 5, 5, &preview),
               "scales a wide image into the requested bounds"))
    return false;
  return require(preview.width == 5 && preview.height == 3,
                 "preserves aspect ratio without exceeding either bound");
}

bool preserves_pixels_without_scaling() {
  const std::array<uint8_t, 8> pixels{12, 34, 56, 78, 90, 123, 210, 255};
  plainroot::filechooser::detail::ImagePreview preview;
  if (!require(plainroot::filechooser::detail::make_image_preview(
                   2, 1, pixels, 8, 8, &preview),
               "accepts an image smaller than the preview bounds"))
    return false;
  return require(preview.width == 2 && preview.height == 1 &&
                     preview.rgba ==
                         std::vector<uint8_t>(pixels.begin(), pixels.end()),
                 "copies unscaled pixels exactly");
}

bool rejects_invalid_input() {
  plainroot::filechooser::detail::ImagePreview preview;
  const std::array<uint8_t, 4> pixel{0, 0, 0, 255};
  return require(!plainroot::filechooser::detail::make_image_preview(
                     0, 1, pixel, 1, 1, &preview),
                 "rejects zero-sized source images") &&
         require(!plainroot::filechooser::detail::make_image_preview(
                     2, 2, pixel, 1, 1, &preview),
                 "rejects truncated source pixel data") &&
         require(!plainroot::filechooser::detail::make_image_preview(
                     1, 1, pixel, 0, 1, &preview),
                 "rejects empty preview bounds");
}

}  // namespace

int main() {
  return bounds_and_aspect_ratio() && preserves_pixels_without_scaling() &&
                 rejects_invalid_input()
             ? 0
             : 1;
}
