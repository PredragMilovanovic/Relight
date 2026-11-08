#include <gtest/gtest.h>

#include <cmath>

#include "relight/layers.hpp"

using namespace relight;

namespace {

// A near square (d = 0.8, red) in front of a far wall (d = 0.2, blue), 100 x 80.
struct Scene {
    ImageF depth{100, 80};
    ImageU8 rgba{100, 80, 4};
    Scene() {
        for (int y = 0; y < 80; ++y)
            for (int x = 0; x < 100; ++x) {
                const bool fg = x >= 40 && x < 70 && y >= 20 && y < 60;
                depth.at(x, y) = fg ? 0.8f : 0.2f;
                rgba.at(x, y, 0) = fg ? 220 : 20;
                rgba.at(x, y, 1) = 30;
                rgba.at(x, y, 2) = fg ? 20 : 200;
                rgba.at(x, y, 3) = 255;
            }
    }
};

}  // namespace

TEST(Layers, SilhouetteMaskMarksOnlyTheNearSide) {
    Scene s;
    const ImageU8 m = silhouette_mask(s.depth, 0.04f);
    EXPECT_EQ(m.at(40, 30), 1);   // left border of the square, foreground side
    EXPECT_EQ(m.at(39, 30), 0);   // background next to it
    EXPECT_EQ(m.at(55, 40), 0);   // inside the square
    EXPECT_EQ(m.at(5, 5), 0);
}

TEST(Layers, PushPullFillsHolesAndKeepsKnownPixels) {
    ImageF v(32, 32, 1), w(32, 32, 1, 1.f);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) v.at(x, y) = static_cast<float>(x);   // a ramp
    for (int y = 10; y < 20; ++y)
        for (int x = 10; x < 20; ++x) {
            v.at(x, y) = -100.f;
            w.at(x, y) = 0.f;
        }
    push_pull_fill(v, w);
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x) {
            if (w.at(x, y) > 0) EXPECT_EQ(v.at(x, y), static_cast<float>(x));
            else EXPECT_NEAR(v.at(x, y), x, 6.0) << x << "," << y;   // smooth guess of the ramp
        }
}

TEST(Layers, BackgroundExtendsBehindTheForeground) {
    Scene s;
    LayerParams p;
    p.occlusion_radius = 8;
    const BackgroundLayer bg = build_background(s.depth, s.rgba, p);
    EXPECT_GT(bg.silhouette_pixels, 0);

    // The band next to the left border is removed and refilled with the wall behind it.
    EXPECT_EQ(bg.occluded.at(42, 40), 1);
    EXPECT_NEAR(bg.depth.at(42, 40), 0.2f, 0.05f);
    EXPECT_LT(bg.color.at(42, 40, 0), 80);    // not red: wall colour
    EXPECT_GT(bg.color.at(42, 40, 2), 150);   // blue

    // The far wall itself is untouched; the band never reaches past the radius.
    EXPECT_EQ(bg.occluded.at(10, 10), 0);
    EXPECT_FLOAT_EQ(bg.depth.at(10, 10), 0.2f);
    EXPECT_EQ(bg.occluded.at(55, 40), 0);     // centre of the square, 15 px from any edge
    // Never nearer than the original anywhere.
    for (std::size_t i = 0; i < bg.depth.data.size(); ++i) EXPECT_LE(bg.depth.data[i], s.depth.data[i]);
}

TEST(Layers, FlatSceneHasNoSilhouettes) {
    ImageF d(50, 50, 1, 0.5f);
    ImageU8 rgba(50, 50, 4, 128);
    const BackgroundLayer bg = build_background(d, rgba, LayerParams{});
    EXPECT_EQ(bg.silhouette_pixels, 0);
    for (std::uint8_t m : bg.occluded.data) EXPECT_EQ(m, 0);
}
