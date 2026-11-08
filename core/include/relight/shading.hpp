// Estimating the photo's own lighting, so it can be removed before relighting.
//
// A photo is roughly albedo x shading. Large-scale shading varies smoothly except where the
// geometry changes, so an edge-preserving smoothing of log-luminance approximates it (a guided
// filter, Retinex-style). Dividing it out gives an albedo estimate without the dark/bright halos
// a plain blur leaves around objects.
#pragma once

#include "relight/image.hpp"

namespace relight {

struct Shading {
    ImageF shading;     // linear luminance of the estimated illumination, > 0
    float mean = 0;     // mean of `shading`, the target brightness after flattening
};

// rgba: sRGB-encoded RGBA8. radius in pixels; eps is the guided filter's regularization in
// (natural log)^2 units: larger values smooth more across texture edges.
Shading estimate_shading(const ImageU8& rgba, int radius, double eps);

}  // namespace relight
