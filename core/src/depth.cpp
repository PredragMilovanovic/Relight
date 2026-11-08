#include "relight/depth.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace relight {

NormalizedDepth normalize_disparity(const float* raw, int width, int height, double low_pct, double high_pct) {
    if (!raw) throw std::invalid_argument("normalize_disparity: null input");
    if (!(low_pct >= 0 && low_pct < high_pct && high_pct <= 1)) {
        throw std::invalid_argument("normalize_disparity: need 0 <= low_pct < high_pct <= 1");
    }
    NormalizedDepth out{ImageF(width, height), 0.f, 1.f};
    const std::size_t n = out.depth.pixels();

    // Percentiles from a strided sample (at most ~50k values) with nth_element: O(n).
    const std::size_t stride = std::max<std::size_t>(1, n / 50000);
    std::vector<float> sample;
    sample.reserve(n / stride + 1);
    for (std::size_t i = 0; i < n; i += stride) {
        if (std::isfinite(raw[i])) sample.push_back(raw[i]);
    }
    if (sample.empty()) return out;  // all NaN: zeros
    const auto pick = [&](double pct) {
        const auto k = static_cast<std::size_t>(std::lround(pct * static_cast<double>(sample.size() - 1)));
        std::nth_element(sample.begin(), sample.begin() + static_cast<std::ptrdiff_t>(k), sample.end());
        return sample[k];
    };
    out.low = pick(low_pct);
    out.high = pick(high_pct);
    const float span = out.high - out.low > 1e-12f ? out.high - out.low : 1.f;
    for (std::size_t i = 0; i < n; ++i) {
        const float v = std::isfinite(raw[i]) ? raw[i] : out.low;
        out.depth.data[i] = std::clamp((v - out.low) / span, 0.f, 1.f);
    }
    return out;
}

ImageF box_mean(const ImageF& src, int radius) {
    if (radius < 0) throw std::invalid_argument("box_mean: negative radius");
    const int w = src.width, h = src.height, c = src.channels;
    ImageF out(w, h, c);
    // Summed-area table in double: float would lose precision over a megapixel.
    std::vector<double> sat(static_cast<std::size_t>(w + 1) * static_cast<std::size_t>(h + 1));
    const auto S = [&](int x, int y) -> double& { return sat[static_cast<std::size_t>(y) * (w + 1) + x]; };
    for (int ch = 0; ch < c; ++ch) {
        for (int y = 0; y < h; ++y) {
            double row = 0;
            for (int x = 0; x < w; ++x) {
                row += src.at(x, y, ch);
                S(x + 1, y + 1) = S(x + 1, y) + row;
            }
        }
        for (int y = 0; y < h; ++y) {
            const int y0 = std::max(0, y - radius), y1 = std::min(h, y + radius + 1);
            for (int x = 0; x < w; ++x) {
                const int x0 = std::max(0, x - radius), x1 = std::min(w, x + radius + 1);
                const double sum = S(x1, y1) - S(x0, y1) - S(x1, y0) + S(x0, y0);
                out.at(x, y, ch) = static_cast<float>(sum / ((x1 - x0) * (y1 - y0)));
            }
        }
    }
    return out;
}

ImageF guided_filter(const ImageF& input, const ImageF& guide, int radius, double eps) {
    if (input.width != guide.width || input.height != guide.height || input.channels != 1 || guide.channels != 1) {
        throw std::invalid_argument("guided_filter: input and guide must be single-channel and the same size");
    }
    if (eps <= 0) throw std::invalid_argument("guided_filter: eps must be positive");
    const std::size_t n = input.pixels();
    ImageF ii(input.width, input.height), ip(input.width, input.height);
    for (std::size_t k = 0; k < n; ++k) {
        ii.data[k] = guide.data[k] * guide.data[k];
        ip.data[k] = guide.data[k] * input.data[k];
    }
    const ImageF mean_i = box_mean(guide, radius);
    const ImageF mean_p = box_mean(input, radius);
    const ImageF corr_i = box_mean(ii, radius);
    const ImageF corr_ip = box_mean(ip, radius);

    ImageF a(input.width, input.height), b(input.width, input.height);
    for (std::size_t k = 0; k < n; ++k) {
        const double var_i = corr_i.data[k] - static_cast<double>(mean_i.data[k]) * mean_i.data[k];
        const double cov_ip = corr_ip.data[k] - static_cast<double>(mean_i.data[k]) * mean_p.data[k];
        const double ak = cov_ip / (std::max(var_i, 0.0) + eps);
        a.data[k] = static_cast<float>(ak);
        b.data[k] = static_cast<float>(mean_p.data[k] - ak * mean_i.data[k]);
    }
    const ImageF mean_a = box_mean(a, radius);
    const ImageF mean_b = box_mean(b, radius);
    ImageF out(input.width, input.height);
    for (std::size_t k = 0; k < n; ++k) out.data[k] = mean_a.data[k] * guide.data[k] + mean_b.data[k];
    return out;
}

namespace {

template <typename Pick>
ImageF extreme_filter(const ImageF& src, int radius, Pick pick) {
    if (radius < 0) throw std::invalid_argument("min/max filter: negative radius");
    if (src.channels != 1) throw std::invalid_argument("min/max filter: single channel only");
    const int w = src.width, h = src.height;
    ImageF tmp(w, h), out(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v = src.at(x, y);
            for (int i = std::max(0, x - radius); i <= std::min(w - 1, x + radius); ++i) v = pick(v, src.at(i, y));
            tmp.at(x, y) = v;
        }
    }
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float v = tmp.at(x, y);
            for (int j = std::max(0, y - radius); j <= std::min(h - 1, y + radius); ++j) v = pick(v, tmp.at(x, j));
            out.at(x, y) = v;
        }
    }
    return out;
}

}  // namespace

ImageF min_filter(const ImageF& src, int radius) {
    return extreme_filter(src, radius, [](float a, float b) { return std::min(a, b); });
}

ImageF max_filter(const ImageF& src, int radius) {
    return extreme_filter(src, radius, [](float a, float b) { return std::max(a, b); });
}

ImageF sharpen_depth_edges(const ImageF& depth, int radius, float threshold) {
    const ImageF lo = min_filter(depth, radius);
    const ImageF hi = max_filter(depth, radius);
    ImageF out = depth;
    for (std::size_t k = 0; k < out.data.size(); ++k) {
        if (hi.data[k] - lo.data[k] <= threshold) continue;
        const float d = depth.data[k];
        out.data[k] = (d - lo.data[k] < hi.data[k] - d) ? lo.data[k] : hi.data[k];
    }
    return out;
}

std::vector<std::uint8_t> encode_depth16(const ImageF& depth) {
    if (depth.channels != 1) throw std::invalid_argument("encode_depth16: single channel only");
    std::vector<std::uint8_t> out(8 + depth.pixels() * 2);
    const auto put32 = [&](std::size_t at, std::uint32_t v) {
        for (int b = 0; b < 4; ++b) out[at + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(v >> (8 * b));
    };
    put32(0, static_cast<std::uint32_t>(depth.width));
    put32(4, static_cast<std::uint32_t>(depth.height));
    for (std::size_t i = 0; i < depth.pixels(); ++i) {
        const auto v = static_cast<std::uint16_t>(std::lround(std::clamp(depth.data[i], 0.f, 1.f) * 65535.f));
        out[8 + 2 * i] = static_cast<std::uint8_t>(v & 0xff);
        out[9 + 2 * i] = static_cast<std::uint8_t>(v >> 8);
    }
    return out;
}

ImageF decode_depth16(const std::uint8_t* bytes, std::size_t size) {
    if (!bytes || size < 8) throw std::invalid_argument("decode_depth16: file too short");
    const auto get32 = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (int b = 0; b < 4; ++b) v |= static_cast<std::uint32_t>(bytes[at + static_cast<std::size_t>(b)]) << (8 * b);
        return v;
    };
    const std::uint32_t w = get32(0), h = get32(4);
    if (w == 0 || h == 0 || w > 16384 || h > 16384 || size != 8 + static_cast<std::size_t>(w) * h * 2) {
        throw std::invalid_argument("decode_depth16: corrupt depth file");
    }
    ImageF img(static_cast<int>(w), static_cast<int>(h));
    for (std::size_t i = 0; i < img.pixels(); ++i) {
        img.data[i] = static_cast<float>(bytes[8 + 2 * i] | (bytes[9 + 2 * i] << 8)) / 65535.f;
    }
    return img;
}

ImageF luminance(const ImageU8& rgba) {
    if (rgba.channels < 3) throw std::invalid_argument("luminance: need at least 3 channels");
    ImageF out(rgba.width, rgba.height);
    for (int y = 0; y < rgba.height; ++y) {
        for (int x = 0; x < rgba.width; ++x) {
            out.at(x, y) = (0.2126f * rgba.at(x, y, 0) + 0.7152f * rgba.at(x, y, 1) + 0.0722f * rgba.at(x, y, 2)) / 255.f;
        }
    }
    return out;
}

}  // namespace relight
