#include "relight/camera.hpp"

#include <algorithm>
#include <cmath>

namespace relight {

double depth_to_view_z(double d, double near_z, double far_z) {
    return 1.0 / (1.0 / far_z + d * (1.0 / near_z - 1.0 / far_z));
}

Vec3 unproject(double u, double v, double d, const Camera& cam) {
    const double z = depth_to_view_z(d, cam.near_z, cam.far_z);
    return {(u * 2.0 - 1.0) * cam.aspect * cam.tan_half_fov * z, (v * 2.0 - 1.0) * cam.tan_half_fov * z, -z};
}

std::array<double, 2> project(const Vec3& p, const Camera& cam) {
    const double z = -p.z;
    return {p.x / (z * cam.aspect * cam.tan_half_fov) * 0.5 + 0.5, p.y / (z * cam.tan_half_fov) * 0.5 + 0.5};
}

Vec3 light_position(double u, double v, double s, double d_surface, const Camera& cam) {
    const double z_lamp = depth_to_view_z(1.15 * (1.0 - std::clamp(s, 0.0, 1.0)), cam.near_z, cam.far_z);
    const double z_surface = depth_to_view_z(d_surface, cam.near_z, cam.far_z);
    const double z = std::min(z_lamp, 0.97 * z_surface);
    return {(u * 2.0 - 1.0) * cam.aspect * cam.tan_half_fov * z, (v * 2.0 - 1.0) * cam.tan_half_fov * z, -z};
}

std::array<double, 3> kelvin_to_rgb(double kelvin) {
    const double t = std::clamp(kelvin, 1000.0, 40000.0) / 100.0;
    double r = 0, g = 0, b = 0;
    if (t <= 66.0) {
        r = 255.0;
        g = 99.4708025861 * std::log(t) - 161.1195681661;
    } else {
        r = 329.698727446 * std::pow(t - 60.0, -0.1332047592);
        g = 288.1221695283 * std::pow(t - 60.0, -0.0755148492);
    }
    if (t >= 66.0) {
        b = 255.0;
    } else if (t <= 19.0) {
        b = 0.0;
    } else {
        b = 138.5177312231 * std::log(t - 10.0) - 305.0447927307;
    }
    std::array<double, 3> rgb{std::clamp(r, 0.0, 255.0) / 255.0, std::clamp(g, 0.0, 255.0) / 255.0,
                              std::clamp(b, 0.0, 255.0) / 255.0};
    const double m = std::max({rgb[0], rgb[1], rgb[2]});
    for (double& c : rgb) c /= m;
    return rgb;
}

double airlight(const Vec3& dir, const Vec3& light, double t_max) {
    const double b = dir.x * light.x + dir.y * light.y + dir.z * light.z;
    const double hx = light.x - b * dir.x, hy = light.y - b * dir.y, hz = light.z - b * dir.z;
    const double h = std::max(std::sqrt(hx * hx + hy * hy + hz * hz), 1e-9);
    return (std::atan((t_max - b) / h) - std::atan(-b / h)) / h;
}

double sample_bilinear(const ImageF& img, double u, double v) {
    const double x = std::clamp(u * img.width - 0.5, 0.0, static_cast<double>(img.width - 1));
    const double y = std::clamp((1.0 - v) * img.height - 0.5, 0.0, static_cast<double>(img.height - 1));
    const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
    const int x1 = std::min(x0 + 1, img.width - 1), y1 = std::min(y0 + 1, img.height - 1);
    const double fx = x - x0, fy = y - y0;
    const double top = img.at(x0, y0) * (1 - fx) + img.at(x1, y0) * fx;
    const double bottom = img.at(x0, y1) * (1 - fx) + img.at(x1, y1) * fx;
    return top * (1 - fy) + bottom * fy;
}

}  // namespace relight
