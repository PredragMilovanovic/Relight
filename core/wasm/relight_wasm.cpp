// WebAssembly bindings (Embind) for the Relight C++ core. Built by scripts/build_wasm.ps1 into
// js/relight_core.js, a single ES module with the binary embedded.
//
// JavaScript usage:
//   const core = await createRelightCore();
//   const scene = new core.Scene();
//   const n = scene.setRawDepth(float32Raw, w, h, 0, 1e-4);   // network output -> normalized
//   scene.setDepth(n, w, h, 5, 1e-4, 7, 0.04);  // guided filter + edge sharpening
//   const bg = scene.buildBackground(rgbaUint8, 0.04, 24);  // { depth, color, occluded }
//   const mesh = scene.buildMesh(400, 0.04);      // { uv, indices, cols, rows }
//   const [x, y, z] = scene.lightPosition(u, v, s, aspect, tanHalfFov, near, far);
//   const gif = new core.GifEncoder(w, h, 6); gif.addFrame(rgba); const bytes = gif.finish();
//   const { shading, mean } = core.estimateShading(rgba, w, h, radius, eps);
//   const view = core.orbitView(yaw, pitch, pivotZ, zoom);   // Float32Array(16), column-major
// Typed arrays returned to JavaScript are copies; they stay valid after the next call.

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "relight/camera.hpp"
#include "relight/depth.hpp"
#include "relight/gif.hpp"
#include "relight/layers.hpp"
#include "relight/mat4.hpp"
#include "relight/mesh.hpp"
#include "relight/shading.hpp"

using emscripten::val;

namespace {

template <typename T>
val copy_to_js(const char* type, const std::vector<T>& v) {
    return val::global(type).new_(emscripten::typed_memory_view(v.size(), v.data()));
}

template <typename T>
std::vector<T> copy_from_js(const val& array) {
    return emscripten::convertJSArrayToNumberVector<T>(array);
}

class Scene {
public:
    // Raw network output -> robust normalization -> self-guided edge-preserving smoothing.
    // Returns the processed depth (row 0 = top).
    val setRawDepth(const val& raw, int width, int height, int radius, double eps) {
        const std::vector<float> data = copy_from_js<float>(raw);
        if (data.size() != static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) {
            throw std::invalid_argument("setRawDepth: size mismatch");
        }
        relight::NormalizedDepth n = relight::normalize_disparity(data.data(), width, height);
        depth_ = radius > 0 ? relight::guided_filter(n.depth, n.depth, radius, eps) : std::move(n.depth);
        for (float& d : depth_.data) d = std::min(1.f, std::max(0.f, d));
        return depth();
    }

    // Normalized depth (fresh from setRawDepth or a cached .depth.bin) -> guided filter
    // (denoise) -> toggle-mapping edge sharpening (blurred silhouettes become steps).
    val setDepth(const val& normalized, int width, int height, int radius, double eps, int sharpen_radius,
                 float edge_threshold) {
        relight::ImageF img(width, height);
        img.data = copy_from_js<float>(normalized);
        if (img.data.size() != img.pixels()) throw std::invalid_argument("setDepth: size mismatch");
        depth_ = radius > 0 ? relight::guided_filter(img, img, radius, eps) : std::move(img);
        if (sharpen_radius > 0) depth_ = relight::sharpen_depth_edges(depth_, sharpen_radius, edge_threshold);
        for (float& d : depth_.data) d = std::min(1.f, std::max(0.f, d));
        return depth();
    }

    val depth() const { return copy_to_js("Float32Array", depth_.data); }
    int width() const { return depth_.width; }
    int height() const { return depth_.height; }

    double sample(double u, double v) const { return relight::sample_bilinear(require(), u, v); }

    val buildBackground(const val& rgba, float edge_threshold, int radius) const {
        const relight::ImageF& d = require();
        relight::ImageU8 img(d.width, d.height, 4);
        img.data = copy_from_js<std::uint8_t>(rgba);
        if (img.data.size() != img.pixels() * 4) throw std::invalid_argument("buildBackground: rgba size mismatch");
        relight::LayerParams p;
        p.edge_threshold = edge_threshold;
        p.occlusion_radius = radius;
        const relight::BackgroundLayer bg = relight::build_background(d, img, p);
        val out = val::object();
        out.set("depth", copy_to_js("Float32Array", bg.depth.data));
        out.set("color", copy_to_js("Uint8Array", bg.color.data));
        out.set("silhouettePixels", bg.silhouette_pixels);
        return out;
    }

    val buildMesh(int max_side, float cut_threshold) const {
        const relight::GridMesh m = relight::build_grid_mesh(require(), max_side, cut_threshold);
        val out = val::object();
        out.set("uv", copy_to_js("Float32Array", m.uv));
        out.set("indices", copy_to_js("Uint32Array", m.indices));
        out.set("cols", m.cols);
        out.set("rows", m.rows);
        return out;
    }

    val lightPosition(double u, double v, double s, double aspect, double tan_half_fov, double near_z, double far_z) const {
        const relight::Camera cam{aspect, tan_half_fov, near_z, far_z};
        const relight::Vec3 p = relight::light_position(u, v, s, sample(u, v), cam);
        val out = val::array();
        out.call<void>("push", p.x, p.y, p.z);
        return out;
    }

    val unproject(double u, double v, double aspect, double tan_half_fov, double near_z, double far_z) const {
        const relight::Camera cam{aspect, tan_half_fov, near_z, far_z};
        const relight::Vec3 p = relight::unproject(u, v, sample(u, v), cam);
        val out = val::array();
        out.call<void>("push", p.x, p.y, p.z);
        return out;
    }

private:
    const relight::ImageF& require() const {
        if (depth_.data.empty()) throw std::logic_error("Scene: no depth set");
        return depth_;
    }
    relight::ImageF depth_;
};

class Gif {
public:
    Gif(int width, int height, int delay_cs) : enc_(width, height, delay_cs) {}
    void addFrame(const val& rgba) {
        const std::vector<std::uint8_t> px = copy_from_js<std::uint8_t>(rgba);
        if (px.size() != static_cast<std::size_t>(enc_.width()) * enc_.height() * 4) {
            throw std::invalid_argument("GifEncoder.addFrame: size mismatch");
        }
        enc_.add_frame(px.data());
    }
    val finish() { return copy_to_js("Uint8Array", enc_.finish()); }
    int frames() const { return enc_.frames(); }

private:
    relight::GifEncoder enc_;
};

val kelvin_to_rgb(double k) {
    const auto c = relight::kelvin_to_rgb(k);
    val out = val::array();
    out.call<void>("push", c[0], c[1], c[2]);
    return out;
}

double airlight(double dx, double dy, double dz, double lx, double ly, double lz, double t_max) {
    return relight::airlight({dx, dy, dz}, {lx, ly, lz}, t_max);
}

// --- matrices (column-major Float32Array(16), ready for gl.uniformMatrix4fv) ---
val mat_to_js(const relight::Mat4& m) {
    return val::global("Float32Array").new_(emscripten::typed_memory_view(m.size(), m.data()));
}
relight::Mat4 mat_from_js(const val& v) {
    const std::vector<float> f = copy_from_js<float>(v);
    if (f.size() != 16) throw std::invalid_argument("expected 16 matrix elements");
    relight::Mat4 m{};
    std::copy(f.begin(), f.end(), m.begin());
    return m;
}
val orbit_view(double yaw, double pitch, double pivot_z, double zoom) {
    return mat_to_js(relight::orbit_view(yaw, pitch, pivot_z, zoom));
}
val perspective(double tan_half_fov, double aspect, double near_z, double far_z) {
    return mat_to_js(relight::perspective(tan_half_fov, aspect, near_z, far_z));
}
val multiply(const val& a, const val& b) { return mat_to_js(relight::multiply(mat_from_js(a), mat_from_js(b))); }
val transform_point(const val& m, double x, double y, double z) {
    const relight::Vec3 p = relight::transform_point(mat_from_js(m), {x, y, z});
    val out = val::array();
    out.call<void>("push", p.x, p.y, p.z);
    return out;
}

// --- cached depth files ---
val encode_depth16(const val& depth, int width, int height) {
    relight::ImageF img(width, height);
    img.data = copy_from_js<float>(depth);
    if (img.data.size() != img.pixels()) throw std::invalid_argument("encodeDepth16: size mismatch");
    return copy_to_js("Uint8Array", relight::encode_depth16(img));
}
val decode_depth16(const val& bytes) {
    const std::vector<std::uint8_t> b = copy_from_js<std::uint8_t>(bytes);
    const relight::ImageF img = relight::decode_depth16(b.data(), b.size());
    val out = val::object();
    out.set("normalized", copy_to_js("Float32Array", img.data));
    out.set("width", img.width);
    out.set("height", img.height);
    return out;
}

// --- illumination estimate for "Remove original lighting" ---
val estimate_shading(const val& rgba, int width, int height, int radius, double eps) {
    relight::ImageU8 img(width, height, 4);
    img.data = copy_from_js<std::uint8_t>(rgba);
    if (img.data.size() != img.pixels() * 4) throw std::invalid_argument("estimateShading: size mismatch");
    const relight::Shading s = relight::estimate_shading(img, radius, eps);
    val out = val::object();
    out.set("shading", copy_to_js("Float32Array", s.shading.data));
    out.set("mean", s.mean);
    return out;
}

std::string version() { return "1.1.0"; }

}  // namespace

EMSCRIPTEN_BINDINGS(relight) {
    emscripten::class_<Scene>("Scene")
        .constructor<>()
        .function("setRawDepth", &Scene::setRawDepth)
        .function("setDepth", &Scene::setDepth)
        .function("depth", &Scene::depth)
        .function("width", &Scene::width)
        .function("height", &Scene::height)
        .function("sample", &Scene::sample)
        .function("buildBackground", &Scene::buildBackground)
        .function("buildMesh", &Scene::buildMesh)
        .function("lightPosition", &Scene::lightPosition)
        .function("unproject", &Scene::unproject);
    emscripten::class_<Gif>("GifEncoder")
        .constructor<int, int, int>()
        .function("addFrame", &Gif::addFrame)
        .function("finish", &Gif::finish)
        .function("frames", &Gif::frames);
    emscripten::function("kelvinToRGB", &kelvin_to_rgb);
    emscripten::function("airlight", &airlight);
    emscripten::function("version", &version);
    emscripten::function("orbitView", &orbit_view);
    emscripten::function("perspective", &perspective);
    emscripten::function("multiply", &multiply);
    emscripten::function("transformPoint", &transform_point);
    emscripten::function("encodeDepth16", &encode_depth16);
    emscripten::function("decodeDepth16", &decode_depth16);
    emscripten::function("estimateShading", &estimate_shading);
}
