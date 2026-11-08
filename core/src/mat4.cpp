#include "relight/mat4.hpp"

#include <cmath>
#include <stdexcept>

namespace relight {

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 o{};
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r) {
            double s = 0;
            for (int k = 0; k < 4; ++k) s += static_cast<double>(a[k * 4 + r]) * b[c * 4 + k];
            o[c * 4 + r] = static_cast<float>(s);
        }
    return o;
}

Mat4 perspective(double tan_half_fov, double aspect, double near_z, double far_z) {
    if (!(tan_half_fov > 0 && aspect > 0 && near_z > 0 && far_z > near_z)) {
        throw std::invalid_argument("perspective: need tan_half_fov, aspect > 0 and 0 < near < far");
    }
    const double f = 1.0 / tan_half_fov, nf = 1.0 / (near_z - far_z);
    Mat4 m{};
    m[0] = static_cast<float>(f / aspect);
    m[5] = static_cast<float>(f);
    m[10] = static_cast<float>((far_z + near_z) * nf);
    m[11] = -1.f;
    m[14] = static_cast<float>(2.0 * far_z * near_z * nf);
    return m;
}

Mat4 translate(double x, double y, double z) {
    Mat4 m = identity();
    m[12] = static_cast<float>(x);
    m[13] = static_cast<float>(y);
    m[14] = static_cast<float>(z);
    return m;
}

Mat4 rotate_x(double a) {
    const auto c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
    return {1, 0, 0, 0, 0, c, s, 0, 0, -s, c, 0, 0, 0, 0, 1};
}

Mat4 rotate_y(double a) {
    const auto c = static_cast<float>(std::cos(a)), s = static_cast<float>(std::sin(a));
    return {c, 0, -s, 0, 0, 1, 0, 0, s, 0, c, 0, 0, 0, 0, 1};
}

Mat4 orbit_view(double yaw, double pitch, double pivot_z, double zoom) {
    return multiply(translate(0, 0, -pivot_z * zoom),
                    multiply(rotate_x(pitch), multiply(rotate_y(yaw), translate(0, 0, pivot_z))));
}

Vec3 transform_point(const Mat4& m, const Vec3& p) {
    const double w = m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15];
    return {(m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12]) / w, (m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13]) / w,
            (m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]) / w};
}

}  // namespace relight
