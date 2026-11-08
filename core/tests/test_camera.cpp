#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "relight/camera.hpp"

using namespace relight;

namespace {
const Camera kCam{1.5, std::tan(55.0 * 3.14159265358979323846 / 360.0), 1.0, 5.0};
}

TEST(Camera, DepthEndpointsAndMonotonic) {
    EXPECT_NEAR(depth_to_view_z(1.0, 1.0, 5.0), 1.0, 1e-12);
    EXPECT_NEAR(depth_to_view_z(0.0, 1.0, 5.0), 5.0, 1e-12);
    double prev = 1e9;
    for (int i = 0; i <= 100; ++i) {
        const double z = depth_to_view_z(i / 100.0, 1.0, 5.0);
        EXPECT_LT(z, prev);
        prev = z;
    }
}

TEST(Camera, ProjectInvertsUnproject) {
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    for (int i = 0; i < 1000; ++i) {
        const double u = U(rng), v = U(rng), d = U(rng);
        const auto p = project(unproject(u, v, d, kCam), kCam);
        EXPECT_NEAR(p[0], u, 1e-12);
        EXPECT_NEAR(p[1], v, 1e-12);
    }
}

TEST(Camera, LampStaysOnItsRayAndInFrontOfTheSurface) {
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    for (int i = 0; i < 1000; ++i) {
        const double u = U(rng), v = U(rng), s = U(rng), ds = U(rng);
        const Vec3 L = light_position(u, v, s, ds, kCam);
        const auto p = project(L, kCam);
        EXPECT_NEAR(p[0], u, 1e-12);
        EXPECT_NEAR(p[1], v, 1e-12);
        EXPECT_LE(-L.z, 0.97 * depth_to_view_z(ds, kCam.near_z, kCam.far_z) + 1e-12);
    }
    EXPECT_LT(-light_position(0.5, 0.5, 0.0, 0.0, kCam).z, -light_position(0.5, 0.5, 0.5, 0.0, kCam).z);
}

TEST(Light, AirlightMatchesNumericIntegration) {
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> U(0.0, 1.0);
    for (int c = 0; c < 50; ++c) {
        Vec3 D{U(rng) - 0.5, U(rng) - 0.5, -1.0};
        const double n = std::sqrt(D.x * D.x + D.y * D.y + D.z * D.z);
        D = {D.x / n, D.y / n, D.z / n};
        const Vec3 L{(U(rng) - 0.5) * 2, (U(rng) - 0.5) * 2, -0.5 - U(rng) * 3};
        const double t_max = 0.5 + U(rng) * 4;
        const auto f = [&](double t) {
            const double x = t * D.x - L.x, y = t * D.y - L.y, z = t * D.z - L.z;
            return 1.0 / (x * x + y * y + z * z);
        };
        const int N = 20000;  // Simpson's rule
        const double h = t_max / N;
        double s = f(0) + f(t_max);
        for (int k = 1; k < N; ++k) s += f(k * h) * (k % 2 ? 4 : 2);
        const double numeric = s * h / 3;
        EXPECT_NEAR(airlight(D, L, t_max) / numeric, 1.0, 1e-6) << "case " << c;
    }
}

TEST(Light, ColourTemperature) {
    const auto d65 = kelvin_to_rgb(6500);
    for (double ch : d65) EXPECT_GT(ch, 0.9);
    const auto warm = kelvin_to_rgb(1900);
    EXPECT_DOUBLE_EQ(warm[0], 1.0);
    EXPECT_LT(warm[1], warm[0]);
    EXPECT_LT(warm[2], warm[1]);
    double prev = -1;
    for (int k = 1500; k <= 6500; k += 250) {
        const auto c = kelvin_to_rgb(k);
        EXPECT_GE(c[2] / c[0], prev) << k << " K";
        prev = c[2] / c[0];
    }
}

TEST(Sampling, BilinearCentresTopRowAndMidpoints) {
    ImageF img(3, 2);
    img.data = {0, 1, 2, 10, 11, 12};
    EXPECT_NEAR(sample_bilinear(img, 0.5 / 3, 1 - 0.5 / 2), 0.0, 1e-12);   // top-left
    EXPECT_NEAR(sample_bilinear(img, 2.5 / 3, 0.5 / 2), 12.0, 1e-12);      // bottom-right
    EXPECT_NEAR(sample_bilinear(img, 1.0 / 3, 0.5), 5.5, 1e-12);           // between four
    EXPECT_NEAR(sample_bilinear(img, -1.0, 2.0), 0.0, 1e-12);              // clamped
}
