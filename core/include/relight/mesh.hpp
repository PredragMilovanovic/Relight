// Grid meshes for the 3D view. Vertices only carry (u, v): the vertex shader reads depth from a
// texture, so the same mesh serves any depth range or field of view.
#pragma once

#include <cstdint>
#include <vector>

#include "relight/image.hpp"

namespace relight {

struct GridMesh {
    int cols = 0;
    int rows = 0;
    std::vector<float> uv;                 // cols * rows * 2, v = 0 at the bottom row
    std::vector<std::uint32_t> indices;    // triangles
};

// Regular grid with at most max_side vertices along the longer side. If cut_threshold > 0, every
// triangle whose vertex disparities differ by more than the threshold is left out: those
// triangles would stretch a "rubber sheet" from a foreground object to the background.
GridMesh build_grid_mesh(const ImageF& depth, int max_side, float cut_threshold);

}  // namespace relight
