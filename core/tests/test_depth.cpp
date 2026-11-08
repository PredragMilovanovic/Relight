#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <random>
#include <vector>

#include "relight/depth.hpp"

using namespace relight;

TEST(Normalize, RangeOutliersAndNaN) {
    const int w = 100, h = 100;
    std::vector<float> raw(w * h);
    for (int i = 0; i < w * h; ++i) raw[i] = 10.f + static_cast<float>(i % 100) / 10.f;
    raw[0] = 1e6f;                                   // a single hot pixel
    raw[1] = std::numeric_limits<float>::quiet_NaN();
    const auto r = normalize_disparity(raw.data(), w, h);
    for (float x : r.depth.data) {
        EXPECT_TRUE(std::isfinite(x));
        EXPECT_GE(x, 0.f);
        EXPECT_LE(x, 1.f);
    }
    EXPECT_NEAR(r.depth.data[50], 0.5f, 0.03f);      // the outlier did not squash the range
}

TEST(Normalize, ConstantInputIsSafe) {
    std::vector<float> raw(64, 3.f);
    const auto r = normalize_disparity(raw.data(), 8, 8);
    for (float x : r.depth.data) EXPECT_TRUE(std::isfinite(x));
}

TEST(BoxMean, MatchesBruteForceIncludingBorders) {
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    ImageF img(17, 11);
    for (float& v : img.data) v = U(rng);
    const int r = 3;
    const ImageF m = box_mean(img, r);
    for (int y = 0; y < img.height; ++y) {
        for (int x = 0; x < img.width; ++x) {
            double s = 0;
            int n = 0;
            for (int j = y - r; j <= y + r; ++j)
                for (int i = x - r; i <= x + r; ++i)
                    if (i >= 0 && j >= 0 && i < img.width && j < img.height) {
                        s += img.at(i, j);
                        ++n;
                    }
            EXPECT_NEAR(m.at(x, y), s / n, 1e-5) << x << "," << y;
        }
    }
}

// A step edge (two flat plateaus) plus noise: the self-guided filter must remove most of the
// noise on the plateaus and keep the step sharp.
TEST(GuidedFilter, DenoisesFlatAreasAndKeepsEdges) {
    const int w = 120, h = 40;
    std::mt19937 rng(9);
    std::normal_distribution<float> noise(0.f, 0.01f);
    ImageF clean(w, h), noisy(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            clean.at(x, y) = x < w / 2 ? 0.3f : 0.8f;
            noisy.at(x, y) = clean.at(x, y) + noise(rng);
        }
    const ImageF out = guided_filter(noisy, noisy, 4, 1e-3);

    const auto rms = [&](const ImageF& a, int x0, int x1) {
        double s = 0;
        int n = 0;
        for (int y = 0; y < h; ++y)
            for (int x = x0; x < x1; ++x) {
                const double d = a.at(x, y) - clean.at(x, y);
                s += d * d;
                ++n;
            }
        return std::sqrt(s / n);
    };
    EXPECT_LT(rms(out, 0, w / 2 - 6), 0.5 * rms(noisy, 0, w / 2 - 6));        // noise reduced
    EXPECT_LT(rms(out, w / 2 + 6, w), 0.5 * rms(noisy, w / 2 + 6, w));
    // The edge stays a step: one pixel on each side is still close to its plateau.
    double left = 0, right = 0;
    for (int y = 0; y < h; ++y) {
        left += out.at(w / 2 - 2, y);
        right += out.at(w / 2 + 1, y);
    }
    EXPECT_NEAR(left / h, 0.3, 0.03);
    EXPECT_NEAR(right / h, 0.8, 0.03);
}

TEST(MinMax, MatchBruteForce) {
    std::mt19937 rng(4);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    ImageF img(13, 9);
    for (float& v : img.data) v = U(rng);
    const int r = 2;
    const ImageF lo = min_filter(img, r), hi = max_filter(img, r);
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            float mn = 2, mx = -1;
            for (int j = std::max(0, y - r); j <= std::min(img.height - 1, y + r); ++j)
                for (int i = std::max(0, x - r); i <= std::min(img.width - 1, x + r); ++i) {
                    mn = std::min(mn, img.at(i, j));
                    mx = std::max(mx, img.at(i, j));
                }
            EXPECT_EQ(lo.at(x, y), mn);
            EXPECT_EQ(hi.at(x, y), mx);
        }
}

// A blurred silhouette (linear ramp 0.2 -> 0.8 over 10 px) must become a clean step, while a
// gentle slope (a floor) and a flat area stay exactly as they were.
TEST(SharpenEdges, RampBecomesStepSlopesUntouched) {
    const int w = 60, h = 4;
    ImageF ramp(w, h), slope(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float t = std::clamp((x - 25) / 10.f, 0.f, 1.f);
            ramp.at(x, y) = 0.2f + 0.6f * t;
            slope.at(x, y) = 0.3f + 0.002f * x;   // 0.002 per pixel
        }
    const ImageF s = sharpen_depth_edges(ramp, 6, 0.04f);
    for (int x = 0; x < w; ++x) {
        const float v = s.at(x, 1);
        EXPECT_TRUE(v == 0.2f || v == 0.8f) << "x=" << x << " v=" << v;   // only plateau values
    }
    EXPECT_EQ(s.at(26, 1), 0.2f);   // near the low end of the ramp -> low side
    EXPECT_EQ(s.at(34, 1), 0.8f);
    const ImageF t = sharpen_depth_edges(slope, 6, 0.04f);
    for (std::size_t k = 0; k < t.data.size(); ++k) EXPECT_EQ(t.data[k], slope.data[k]);
}

// With the app's settings (radius 4, threshold 0.12) a steep but smooth surface (0.01 per pixel,
// steeper than any real floor or wall in a normalized depth map) must not be cut into terraces.
TEST(SharpenEdges, AppSettingsLeaveSteepSmoothSurfaces) {
    ImageF steep(200, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 200; ++x) steep.at(x, y) = 0.01f * x * 0.5f + 0.0001f * x * x * 0.1f;
    const ImageF s = sharpen_depth_edges(steep, 4, 0.12f);
    for (std::size_t k = 0; k < s.data.size(); ++k) EXPECT_EQ(s.data[k], steep.data[k]);
    // ...but a silhouette blurred over 8 px (0.5 jump) still becomes a step.
    ImageF edge(60, 3);
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 60; ++x) edge.at(x, y) = 0.3f + 0.5f * std::clamp((x - 26) / 8.f, 0.f, 1.f);
    const ImageF e = sharpen_depth_edges(edge, 4, 0.12f);
    for (int x = 0; x < 60; ++x) EXPECT_TRUE(e.at(x, 1) == 0.3f || e.at(x, 1) == 0.8f) << x;
}

TEST(GuidedFilter, RejectsBadInput) {
    EXPECT_THROW(guided_filter(ImageF(4, 4), ImageF(5, 4), 1, 1e-3), std::invalid_argument);
    EXPECT_THROW(guided_filter(ImageF(4, 4), ImageF(4, 4), 1, 0.0), std::invalid_argument);
}
