#include "relight/layers.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <stdexcept>

namespace relight {

namespace {

constexpr int kDx4[4] = {1, -1, 0, 0};
constexpr int kDy4[4] = {0, 0, 1, -1};

// One level of push-pull. Weights are the averaging kernel; 0 marks a pixel to fill.
void fill_level(ImageF& v, const ImageF& w) {
    const int W = v.width, H = v.height, C = v.channels;
    if (std::none_of(w.data.begin(), w.data.end(), [](float x) { return x <= 0.f; })) return;
    if (W == 1 && H == 1) return;  // nothing left to pull from

    // Pull: weighted 2x2 average into a half-size level.
    const int cw = (W + 1) / 2, ch = (H + 1) / 2;
    ImageF cv(cw, ch, C), cwt(cw, ch);
    for (int Y = 0; Y < ch; ++Y) {
        for (int X = 0; X < cw; ++X) {
            double ws = 0;
            double acc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (int j = 0; j < 2; ++j) {
                for (int i = 0; i < 2; ++i) {
                    const int x = 2 * X + i, y = 2 * Y + j;
                    if (x >= W || y >= H) continue;
                    const double wk = w.at(x, y);
                    if (wk <= 0) continue;
                    ws += wk;
                    for (int c = 0; c < C; ++c) acc[c] += wk * v.at(x, y, c);
                }
            }
            cwt.at(X, Y) = static_cast<float>(ws);
            if (ws > 0) {
                for (int c = 0; c < C; ++c) cv.at(X, Y, c) = static_cast<float>(acc[c] / ws);
            }
        }
    }
    fill_level(cv, cwt);

    // Push: holes take the bilinear upsample of the (now complete) coarser level.
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (w.at(x, y) > 0) continue;
            const double fx = std::clamp((x + 0.5) / 2.0 - 0.5, 0.0, cw - 1.0);
            const double fy = std::clamp((y + 0.5) / 2.0 - 0.5, 0.0, ch - 1.0);
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const int x1 = std::min(x0 + 1, cw - 1), y1 = std::min(y0 + 1, ch - 1);
            const double ax = fx - x0, ay = fy - y0;
            for (int c = 0; c < C; ++c) {
                const double top = cv.at(x0, y0, c) * (1 - ax) + cv.at(x1, y0, c) * ax;
                const double bot = cv.at(x0, y1, c) * (1 - ax) + cv.at(x1, y1, c) * ax;
                v.at(x, y, c) = static_cast<float>(top * (1 - ay) + bot * ay);
            }
        }
    }
}

}  // namespace

ImageU8 silhouette_mask(const ImageF& depth, float threshold) {
    ImageU8 mask(depth.width, depth.height);
    for (int y = 0; y < depth.height; ++y) {
        for (int x = 0; x < depth.width; ++x) {
            const float d = depth.at(x, y);
            for (int k = 0; k < 4; ++k) {
                const int nx = x + kDx4[k], ny = y + kDy4[k];
                if (nx < 0 || ny < 0 || nx >= depth.width || ny >= depth.height) continue;
                if (d - depth.at(nx, ny) > threshold) {
                    mask.at(x, y) = 1;
                    break;
                }
            }
        }
    }
    return mask;
}

void push_pull_fill(ImageF& values, const ImageF& weights) {
    if (values.width != weights.width || values.height != weights.height || weights.channels != 1) {
        throw std::invalid_argument("push_pull_fill: weights must be single-channel and match the values");
    }
    if (values.channels > 8) throw std::invalid_argument("push_pull_fill: at most 8 channels");
    fill_level(values, weights);
}

BackgroundLayer build_background(const ImageF& depth, const ImageU8& rgba, const LayerParams& params) {
    if (depth.width != rgba.width || depth.height != rgba.height || rgba.channels != 4) {
        throw std::invalid_argument("build_background: rgba must be RGBA8 and the same size as the depth");
    }
    const int W = depth.width, H = depth.height;
    const float t = params.edge_threshold;

    // 1. Seeds: near-side silhouette pixels, each remembering the background disparity behind it.
    ImageF far_level(W, H, 1, -1.f);
    std::deque<std::pair<int, int>> queue;  // (pixel index, distance in steps)
    BackgroundLayer out{ImageF(W, H), ImageU8(W, H, 4), ImageU8(W, H), 0};
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float d = depth.at(x, y);
            float far = 2.f;
            for (int k = 0; k < 4; ++k) {
                const int nx = x + kDx4[k], ny = y + kDy4[k];
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                if (d - depth.at(nx, ny) > t) far = std::min(far, depth.at(nx, ny));
            }
            if (far < 2.f) {
                far_level.at(x, y) = far;
                out.occluded.at(x, y) = 1;
                queue.emplace_back(y * W + x, 0);
                ++out.silhouette_pixels;
            }
        }
    }

    // 2. Grow the removed band into the foreground (breadth-first, 8-connected, up to the radius),
    //    only through pixels that are still clearly nearer than the background behind the seed.
    while (!queue.empty()) {
        const auto [idx, dist] = queue.front();
        queue.pop_front();
        if (dist >= params.occlusion_radius) continue;
        const int x = idx % W, y = idx / W;
        const float far = far_level.at(x, y);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const int nx = x + dx, ny = y + dy;
                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                if (out.occluded.at(nx, ny) || depth.at(nx, ny) <= far + 0.5f * t) continue;
                out.occluded.at(nx, ny) = 1;
                far_level.at(nx, ny) = far;
                queue.emplace_back(ny * W + nx, dist + 1);
            }
        }
    }

    // 3. Inpaint depth and colour of the removed band by propagation from the background side:
    //    a band pixel is filled with the mean of its already-known neighbours, where "known" means
    //    an original pixel that lies behind the band's foreground (disparity <= far + t/2) or a
    //    band pixel filled earlier. Breadth-first order makes the fill grow inwards from the
    //    background, so the foreground's own interior never leaks into it.
    ImageF values(W, H, 4);
    ImageU8 known(W, H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            values.at(x, y, 0) = depth.at(x, y);
            for (int c = 0; c < 3; ++c) values.at(x, y, c + 1) = rgba.at(x, y, c) / 255.f;
        }
    }
    const auto usable = [&](int x, int y, float far) {
        if (known.at(x, y)) return true;
        return !out.occluded.at(x, y) && depth.at(x, y) <= far + 0.5f * t;
    };
    std::deque<int> front;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (out.occluded.at(x, y)) front.push_back(y * W + x);
    // Repeated sweeps over the remaining band; each sweep fills the pixels touching known ones.
    std::vector<int> ready;
    for (bool progress = true; progress && !front.empty();) {
        progress = false;
        ready.clear();
        for (int idx : front) {
            const int x = idx % W, y = idx / W;
            const float far = far_level.at(x, y);
            double acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= W || ny >= H || !usable(nx, ny, far)) continue;
                    for (int c = 0; c < 4; ++c) acc[c] += values.at(nx, ny, c);
                    ++n;
                }
            if (n == 0) continue;
            for (int c = 0; c < 4; ++c) values.at(x, y, c) = static_cast<float>(acc[c] / n);
            ready.push_back(idx);
        }
        for (int idx : ready) known.data[static_cast<std::size_t>(idx)] = 1;  // after the sweep: no order bias
        if (!ready.empty()) {
            progress = true;
            std::deque<int> rest;
            for (int idx : front)
                if (!known.data[static_cast<std::size_t>(idx)]) rest.push_back(idx);
            front.swap(rest);
        }
    }
    // Band pixels with no background anywhere near (rare): push-pull from everything, far first.
    if (!front.empty()) {
        ImageF weights(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const bool hole = out.occluded.at(x, y) && !known.at(x, y);
                weights.at(x, y) = hole ? 0.f : static_cast<float>(std::pow(1.0 - values.at(x, y, 0), params.far_bias)) + 1e-12f;
            }
        push_pull_fill(values, weights);
    }

    // 4. Propagation leaves streaks along its direction; a few 3x3 averaging passes restricted to
    //    the band smooth them (the original pixels are never touched).
    for (int pass = 0; pass < params.smooth_passes; ++pass) {
        const ImageF prev = values;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                if (!out.occluded.at(x, y)) continue;
                double acc[4] = {0, 0, 0, 0};
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                        if (!out.occluded.at(nx, ny) && depth.at(nx, ny) > far_level.at(x, y) + 0.5f * t) continue;
                        for (int c = 0; c < 4; ++c) acc[c] += prev.at(nx, ny, c);
                        ++n;
                    }
                for (int c = 0; c < 4; ++c) values.at(x, y, c) = static_cast<float>(acc[c] / n);
            }
    }

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            out.depth.at(x, y) = std::min(values.at(x, y, 0), depth.at(x, y));  // never nearer
            for (int c = 0; c < 3; ++c) {
                out.color.at(x, y, c) =
                    static_cast<std::uint8_t>(std::lround(std::clamp(values.at(x, y, c + 1), 0.f, 1.f) * 255.f));
            }
            out.color.at(x, y, 3) = 255;
        }
    }
    return out;
}

}  // namespace relight
