// Camera model, lamp placement and light helpers.
//
// Conventions
//   uv    texture coordinates of the photo, (0,0) bottom-left, (1,1) top-right
//   d     normalized disparity from the depth network: 0 = farthest, 1 = nearest
//   view  right-handed camera space, camera at the origin looking down -z
//   z     positive distance along the viewing axis (view-space z = -z)
// The GLSL in js/shaders.js mirrors these functions exactly.
#pragma once

#include <array>

#include "relight/image.hpp"

namespace relight {

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

struct Camera {
    double aspect = 1.5;        // width / height of the photo
    double tan_half_fov = 0.5;  // vertical
    double near_z = 1.0;
    double far_z = 5.0;
};

// Monocular networks predict relative inverse depth, so d is interpreted linearly in 1/z:
//   1/z = 1/far + d (1/near - 1/far)
double depth_to_view_z(double d, double near_z, double far_z);

Vec3 unproject(double u, double v, double d, const Camera& cam);
std::array<double, 2> project(const Vec3& p, const Camera& cam);

// A lamp is placed by where it appears in the photo (u, v) and a depth slider s in [0, 1]
// (0 = just in front of the nearest object, 1 = at the back). It stays on the camera ray through
// (u, v) and never goes behind the surface under it (at most 97 % of that surface's distance).
Vec3 light_position(double u, double v, double s, double d_surface, const Camera& cam);

// Correlated colour temperature -> RGB in [0, 1], max channel = 1 (Planckian locus curve fit,
// valid 1000 K .. 40000 K).
std::array<double, 3> kelvin_to_rgb(double kelvin);

// In-scattered light of an isotropic point source L seen along the ray t * dir, t in [0, t_max]:
//   A = integral dt / |t dir - L|^2 = [atan((t_max - b)/h) - atan(-b/h)] / h
// with b = dir . L and h = |L - b dir|. dir must be a unit vector.
double airlight(const Vec3& dir, const Vec3& light, double t_max);

// Bilinear sample of channel 0 at (u, v), v = 1 is the top row; clamped at the borders.
double sample_bilinear(const ImageF& img, double u, double v);

}  // namespace relight
