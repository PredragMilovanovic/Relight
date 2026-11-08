#include "relight/shading.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "relight/depth.hpp"

namespace relight {

namespace {

float srgb_to_linear(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }

}  // namespace

Shading estimate_shading(const ImageU8& rgba, int radius, double eps) {
    if (rgba.channels != 4) throw std::invalid_argument("estimate_shading: RGBA8 expected");
    if (radius < 1) throw std::invalid_argument("estimate_shading: radius must be >= 1");

    float lut[256];
    for (int i = 0; i < 256; ++i) lut[i] = srgb_to_linear(i / 255.f);

    // Log of linear luminance; the floor keeps black pixels finite.
    ImageF log_y(rgba.width, rgba.height);
    for (int y = 0; y < rgba.height; ++y)
        for (int x = 0; x < rgba.width; ++x) {
            const float Y = 0.2126f * lut[rgba.at(x, y, 0)] + 0.7152f * lut[rgba.at(x, y, 1)] + 0.0722f * lut[rgba.at(x, y, 2)];
            log_y.at(x, y) = std::log(std::max(Y, 1e-3f));
        }

    // Two passes: a wide self-guided filter, then a second one to flatten remaining texture.
    const ImageF smooth = guided_filter(guided_filter(log_y, log_y, radius, eps), log_y, radius, eps);

    Shading out{ImageF(rgba.width, rgba.height), 0.f};
    double sum = 0;
    for (std::size_t k = 0; k < smooth.data.size(); ++k) {
        out.shading.data[k] = std::exp(smooth.data[k]);
        sum += out.shading.data[k];
    }
    out.mean = static_cast<float>(sum / static_cast<double>(smooth.data.size()));
    return out;
}

}  // namespace relight
