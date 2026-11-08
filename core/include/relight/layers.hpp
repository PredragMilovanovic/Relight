// Layered depth for the 3D view.
//
// A single photo has no pixels for what is hidden behind a foreground object. When the 3D view
// turns, the foreground moves more than the background and uncovers a gap. To fill it we build a
// background layer: the foreground band next to every silhouette is removed and the background's
// depth and colour are extended into it (push-pull inpainting biased towards far pixels). The 3D
// view draws this layer first and the original, cut at silhouettes, on top.
#pragma once

#include <cstdint>
#include <vector>

#include "relight/image.hpp"

namespace relight {

struct LayerParams {
    float edge_threshold = 0.04f;  // disparity jump between neighbours that counts as a silhouette
    int occlusion_radius = 24;     // pixels of foreground removed next to each silhouette
    float far_bias = 16.0f;        // fallback inpainting weight (1 - d)^far_bias: far pixels dominate
    int smooth_passes = 12;        // 3x3 averaging passes over the filled band (removes streaks)
};

struct BackgroundLayer {
    ImageF depth;           // background disparity (never nearer than the original)
    ImageU8 color;          // RGBA8, background colour extended under the foreground band
    ImageU8 occluded;       // 1 where the foreground was removed
    int silhouette_pixels = 0;
};

// 1 for pixels on the NEAR side of a disparity jump larger than threshold to a 4-neighbour.
ImageU8 silhouette_mask(const ImageF& depth, float threshold);

// Fills pixels whose weight is 0 from the surrounding weighted pixels (push-pull / pyramid
// interpolation). Known pixels (weight > 0) are left unchanged. `values` may have any number of
// channels; `weights` has one.
void push_pull_fill(ImageF& values, const ImageF& weights);

BackgroundLayer build_background(const ImageF& depth, const ImageU8& rgba, const LayerParams& params);

}  // namespace relight
