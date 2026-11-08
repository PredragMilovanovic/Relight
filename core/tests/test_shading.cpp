#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "relight/depth.hpp"
#include "relight/shading.hpp"

using namespace relight;

namespace {
float lin(std::uint8_t c) {
    const float v = c / 255.f;
    return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
}
std::uint8_t enc(float v) {
    v = std::clamp(v, 0.f, 1.f);
    const float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - 0.055f;
    return static_cast<std::uint8_t>(std::lround(s * 255));
}
}  // namespace

// A fine checkerboard (albedo 0.2 / 0.5) under illumination that brightens 4x from left to right.
// After dividing out the estimated shading, the same albedo must look (nearly) the same on both
// sides, while the texture itself survives.
TEST(Shading, RemovesSmoothIlluminationKeepsTexture) {
    const int w = 240, h = 80;
    ImageU8 img(w, h, 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float albedo = ((x / 4 + y / 4) % 2) ? 0.5f : 0.2f;
            const float light = 0.25f + 0.75f * x / (w - 1);
            const std::uint8_t c = enc(albedo * light);
            img.at(x, y, 0) = img.at(x, y, 1) = img.at(x, y, 2) = c;
            img.at(x, y, 3) = 255;
        }
    const Shading s = estimate_shading(img, 12, 0.5);
    ASSERT_GT(s.mean, 0.f);
    const auto albedo_at = [&](int x, int y) { return lin(img.at(x, y, 0)) / s.shading.at(x, y); };
    // Bright cells (albedo 0.5) are those where (x/4 + y/4) is odd: x = 29 (cell 7), x = 213 (cell 53).
    const double left = albedo_at(29, 1), right = albedo_at(213, 1);
    const double before = lin(img.at(213, 1, 0)) / lin(img.at(29, 1, 0));   // ~3.3x brighter
    EXPECT_GT(before, 2.5);
    EXPECT_LT(std::abs(right / left - 1.0), 0.25) << "left " << left << " right " << right;
    // Texture survives: neighbouring cells still differ by about 2.5x.
    EXPECT_GT(albedo_at(213, 1) / albedo_at(209, 1), 1.8);
}

TEST(DepthFile, RoundTripAndCorruption) {
    ImageF d(37, 23);
    for (std::size_t i = 0; i < d.data.size(); ++i) d.data[i] = static_cast<float>((i * 7919) % 1000) / 999.f;
    const auto bytes = encode_depth16(d);
    ASSERT_EQ(bytes.size(), 8u + 37u * 23u * 2u);
    const ImageF back = decode_depth16(bytes.data(), bytes.size());
    ASSERT_EQ(back.width, 37);
    ASSERT_EQ(back.height, 23);
    for (std::size_t i = 0; i < d.data.size(); ++i) EXPECT_NEAR(back.data[i], d.data[i], 0.5 / 65535 + 1e-7);
    EXPECT_THROW(decode_depth16(bytes.data(), 100), std::invalid_argument);
    EXPECT_THROW(decode_depth16(bytes.data(), 4), std::invalid_argument);
}
