// Turning raw network output into clean, usable depth.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "relight/image.hpp"

namespace relight {

struct NormalizedDepth {
    ImageF depth;      // in [0, 1], 1 = nearest
    float low = 0;     // raw value mapped to 0
    float high = 1;    // raw value mapped to 1
};

// Robust min-max normalization: the lo/hi percentiles map to 0/1 and are clipped, so single
// outlier pixels (a highlight, a patch of sky) cannot squash the useful range. Non-finite values
// are treated as the low percentile.
NormalizedDepth normalize_disparity(const float* raw, int width, int height, double low_pct = 0.005,
                                    double high_pct = 0.995);

// Mean over the (2r+1)^2 window, clipped at the borders (each pixel divides by the number of
// pixels actually inside its window). O(1) per pixel via a summed-area table.
ImageF box_mean(const ImageF& src, int radius);

// Guided filter (He, Sun, Tang 2010), single-channel guide. Edge-preserving smoothing: where the
// guide varies much more than sqrt(eps) inside a window the output follows it, where it is flat
// the input is averaged. With guide = input it denoises depth without blurring silhouettes.
ImageF guided_filter(const ImageF& input, const ImageF& guide, int radius, double eps);

// Min / max over the (2r+1)^2 window (separable, clipped at the borders).
ImageF min_filter(const ImageF& src, int radius);
ImageF max_filter(const ImageF& src, int radius);

// Edge sharpening by toggle mapping. Monocular networks blur silhouettes into ramps several
// pixels wide; in 3D those ramps become stretched sheets and in the normals a halo. Where the
// local range (max - min over the window) exceeds `threshold`, each pixel snaps to the nearer of
// the local min and max, so a ramp becomes a step. Smooth slopes (range below the threshold) and
// flat areas are returned unchanged.
ImageF sharpen_depth_edges(const ImageF& depth, int radius, float threshold);

// Cached depth file (".depth.bin"): uint32 width, uint32 height (little endian), then
// width*height uint16 values (d * 65535), row 0 = top. 16 bits, because 8-bit depth shows
// terraces in the normals. decode throws on truncated or inconsistent input.
std::vector<std::uint8_t> encode_depth16(const ImageF& depth);
ImageF decode_depth16(const std::uint8_t* bytes, std::size_t size);

// Rec. 709 luminance of an RGBA8 image, in [0, 1] (no linearization; used as a guide only).
ImageF luminance(const ImageU8& rgba);

}  // namespace relight
