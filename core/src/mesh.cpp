#include "relight/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "relight/camera.hpp"

namespace relight {

GridMesh build_grid_mesh(const ImageF& depth, int max_side, float cut_threshold) {
    if (max_side < 2) throw std::invalid_argument("build_grid_mesh: max_side must be at least 2");
    const double s = std::min(1.0, static_cast<double>(max_side) / std::max(depth.width, depth.height));
    GridMesh m;
    m.cols = std::max(2, static_cast<int>(std::lround(depth.width * s)));
    m.rows = std::max(2, static_cast<int>(std::lround(depth.height * s)));
    m.uv.resize(static_cast<std::size_t>(m.cols) * m.rows * 2);
    std::vector<float> d(static_cast<std::size_t>(m.cols) * m.rows);
    for (int y = 0; y < m.rows; ++y) {
        for (int x = 0; x < m.cols; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * m.cols + x;
            const double u = static_cast<double>(x) / (m.cols - 1), v = static_cast<double>(y) / (m.rows - 1);
            m.uv[2 * i] = static_cast<float>(u);
            m.uv[2 * i + 1] = static_cast<float>(v);
            d[i] = static_cast<float>(sample_bilinear(depth, u, v));
        }
    }
    m.indices.reserve(static_cast<std::size_t>(m.cols - 1) * (m.rows - 1) * 6);
    const auto keep = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
        if (cut_threshold > 0) {
            const float lo = std::min({d[a], d[b], d[c]}), hi = std::max({d[a], d[b], d[c]});
            if (hi - lo > cut_threshold) return;
        }
        m.indices.insert(m.indices.end(), {a, b, c});
    };
    for (int y = 0; y + 1 < m.rows; ++y) {
        for (int x = 0; x + 1 < m.cols; ++x) {
            const auto a = static_cast<std::uint32_t>(y * m.cols + x);
            const std::uint32_t b = a + 1, c = a + static_cast<std::uint32_t>(m.cols), e = c + 1;
            keep(a, b, e);
            keep(a, e, c);
        }
    }
    return m;
}

}  // namespace relight
