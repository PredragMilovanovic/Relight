#include <gtest/gtest.h>

#include <cmath>
#include <random>

#include "relight/mat4.hpp"

using namespace relight;

TEST(Mat4, PerspectiveMapsNearAndFarToNdc) {
    const Mat4 P = perspective(0.5, 1.5, 0.1, 50);
    EXPECT_NEAR(transform_point(P, {0, 0, -0.1}).z, -1.0, 1e-5);
    EXPECT_NEAR(transform_point(P, {0, 0, -50}).z, 1.0, 1e-4);
    EXPECT_NEAR(transform_point(P, {0, 1, -2}).y, 1.0, 1e-6);       // y = tan * z is the top edge
    EXPECT_NEAR(transform_point(P, {1.5, 0, -2}).x, 1.0, 1e-6);     // x = aspect * tan * z
    EXPECT_THROW(perspective(0.5, 1.5, 1.0, 0.5), std::invalid_argument);
}

TEST(Mat4, MultiplyIsAssociativeAndIdentityIsNeutral) {
    std::mt19937 rng(8);
    std::uniform_real_distribution<float> U(-1, 1);
    Mat4 a, b, c;
    for (int i = 0; i < 16; ++i) { a[i] = U(rng); b[i] = U(rng); c[i] = U(rng); }
    const Mat4 l = multiply(multiply(a, b), c), r = multiply(a, multiply(b, c));
    for (int i = 0; i < 16; ++i) EXPECT_NEAR(l[i], r[i], 1e-5);
    const Mat4 ai = multiply(a, identity());
    for (int i = 0; i < 16; ++i) EXPECT_FLOAT_EQ(ai[i], a[i]);
}

TEST(Mat4, RotationsKeepLengths) {
    const Vec3 p{0.3, -0.7, 1.1};
    const double len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    for (double a : {0.1, 1.0, 2.5}) {
        for (const Mat4& R : {rotate_x(a), rotate_y(a)}) {
            const Vec3 q = transform_point(R, p);
            EXPECT_NEAR(std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z), len, 1e-6);
        }
    }
}

TEST(Mat4, OrbitIsIdentityAtRestAndKeepsThePivotFixed) {
    const Vec3 p{0.3, -0.2, -2.5};
    const Vec3 q = transform_point(orbit_view(0, 0, 3, 1), p);
    EXPECT_NEAR(q.x, p.x, 1e-6);
    EXPECT_NEAR(q.y, p.y, 1e-6);
    EXPECT_NEAR(q.z, p.z, 1e-6);
    std::mt19937 rng(9);
    std::uniform_real_distribution<double> U(-0.5, 0.5);
    for (int i = 0; i < 100; ++i) {
        const Vec3 c = transform_point(orbit_view(U(rng), U(rng), 3, 1), {0, 0, -3});
        EXPECT_NEAR(std::sqrt(c.x * c.x + c.y * c.y + (c.z + 3) * (c.z + 3)), 0.0, 1e-5);
    }
    // Zooming moves the pivot along the axis only.
    const Vec3 z = transform_point(orbit_view(0.2, 0.1, 3, 1.5), {0, 0, -3});
    EXPECT_NEAR(z.z, -4.5, 1e-5);
}
