// 4x4 matrices for the WebGL view: column-major float storage (OpenGL / GLM layout), so a
// Mat4 can be passed straight to gl.uniformMatrix4fv.
#pragma once

#include <array>

#include "relight/camera.hpp"

namespace relight {

using Mat4 = std::array<float, 16>;

Mat4 identity();
Mat4 multiply(const Mat4& a, const Mat4& b);                      // a * b
Mat4 perspective(double tan_half_fov, double aspect, double near_z, double far_z);
Mat4 translate(double x, double y, double z);
Mat4 rotate_x(double radians);
Mat4 rotate_y(double radians);

// Orbit camera around a pivot on the optical axis at distance pivot_z:
//   view = T(0,0,-pivot_z*zoom) * Rx(pitch) * Ry(yaw) * T(0,0,+pivot_z)
// yaw = pitch = 0 and zoom = 1 give the identity: the 3D view starts exactly at the photo.
Mat4 orbit_view(double yaw, double pitch, double pivot_z, double zoom);

// Transforms a point and divides by w.
Vec3 transform_point(const Mat4& m, const Vec3& p);

}  // namespace relight
