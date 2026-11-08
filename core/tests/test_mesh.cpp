#include <gtest/gtest.h>

#include <algorithm>

#include "relight/camera.hpp"
#include "relight/mesh.hpp"

using namespace relight;

TEST(Mesh, FullGridSizeAndUvRange) {
    ImageF d(800, 400, 1, 0.5f);
    const GridMesh m = build_grid_mesh(d, 200, 0.f);
    EXPECT_EQ(m.cols, 200);
    EXPECT_EQ(m.rows, 100);
    EXPECT_EQ(m.indices.size(), static_cast<std::size_t>(199 * 99 * 6));
    EXPECT_FLOAT_EQ(m.uv.front(), 0.f);
    EXPECT_FLOAT_EQ(m.uv.back(), 1.f);
    for (std::uint32_t i : m.indices) EXPECT_LT(i, static_cast<std::uint32_t>(m.cols * m.rows));
}

TEST(Mesh, CutsExactlyTheTrianglesAcrossADepthStep) {
    ImageF d(100, 100);
    for (int y = 0; y < 100; ++y)
        for (int x = 0; x < 100; ++x) d.at(x, y) = x < 50 ? 0.8f : 0.2f;
    const GridMesh full = build_grid_mesh(d, 100, 0.f);
    const GridMesh cut = build_grid_mesh(d, 100, 0.05f);
    EXPECT_LT(cut.indices.size(), full.indices.size());
    // Every kept triangle lies on one plateau.
    for (std::size_t t = 0; t < cut.indices.size(); t += 3) {
        float lo = 1, hi = 0;
        for (int k = 0; k < 3; ++k) {
            const std::uint32_t i = cut.indices[t + k];
            const float v = static_cast<float>(sample_bilinear(d, cut.uv[2 * i], cut.uv[2 * i + 1]));
            lo = std::min(lo, v);
            hi = std::max(hi, v);
        }
        EXPECT_LE(hi - lo, 0.05f);
    }
    // Only the one column of quads that straddles the step is removed (2 triangles per row).
    EXPECT_EQ(full.indices.size() - cut.indices.size(), static_cast<std::size_t>(99 * 2 * 3));
}
