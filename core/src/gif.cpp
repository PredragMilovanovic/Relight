#include "relight/gif.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>

namespace relight {

namespace {

std::uint32_t pack(const Rgb& c) { return (std::uint32_t{c[0]} << 16) | (std::uint32_t{c[1]} << 8) | c[2]; }

struct Box {
    std::size_t begin, end;  // range in the pixel vector
    int channel = 0;         // channel with the widest range
    int range = 0;
};

Box measure(const std::vector<Rgb>& px, std::size_t begin, std::size_t end) {
    std::uint8_t lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (std::size_t i = begin; i < end; ++i) {
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], px[i][c]);
            hi[c] = std::max(hi[c], px[i][c]);
        }
    }
    Box b{begin, end, 0, -1};
    for (int c = 0; c < 3; ++c) {
        if (hi[c] - lo[c] > b.range) {
            b.range = hi[c] - lo[c];
            b.channel = c;
        }
    }
    return b;
}

// 4x4 Bayer matrix, thresholds in (-0.5, 0.5).
constexpr float kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

}  // namespace

std::vector<Rgb> median_cut_palette(const std::uint8_t* rgba, std::size_t pixel_count, int max_colors) {
    if (!rgba || pixel_count == 0) throw std::invalid_argument("median_cut_palette: empty image");
    if (max_colors < 1 || max_colors > 256) throw std::invalid_argument("median_cut_palette: 1..256 colours");

    // At most ~64k pixels take part; enough for a stable palette, and fast.
    const std::size_t stride = std::max<std::size_t>(1, pixel_count / 65536);
    std::vector<Rgb> px;
    px.reserve(pixel_count / stride + 1);
    std::unordered_set<std::uint32_t> distinct;
    for (std::size_t i = 0; i < pixel_count; i += stride) {
        const Rgb c{rgba[4 * i], rgba[4 * i + 1], rgba[4 * i + 2]};
        px.push_back(c);
        if (distinct.size() <= static_cast<std::size_t>(max_colors)) distinct.insert(pack(c));
    }
    if (stride == 1 && distinct.size() <= static_cast<std::size_t>(max_colors)) {
        std::vector<std::uint32_t> keys(distinct.begin(), distinct.end());
        std::sort(keys.begin(), keys.end());
        std::vector<Rgb> exact;
        for (std::uint32_t k : keys) {
            exact.push_back({static_cast<std::uint8_t>(k >> 16), static_cast<std::uint8_t>(k >> 8), static_cast<std::uint8_t>(k)});
        }
        return exact;
    }

    std::vector<Box> boxes{measure(px, 0, px.size())};
    while (static_cast<int>(boxes.size()) < max_colors) {
        // Split the box with the widest channel range (ties: more pixels).
        auto it = std::max_element(boxes.begin(), boxes.end(), [](const Box& a, const Box& b) {
            return a.range != b.range ? a.range < b.range : (a.end - a.begin) < (b.end - b.begin);
        });
        if (it->range <= 0 || it->end - it->begin < 2) break;
        const Box b = *it;
        const std::size_t mid = b.begin + (b.end - b.begin) / 2;
        const int c = b.channel;
        std::nth_element(px.begin() + static_cast<std::ptrdiff_t>(b.begin), px.begin() + static_cast<std::ptrdiff_t>(mid),
                         px.begin() + static_cast<std::ptrdiff_t>(b.end),
                         [c](const Rgb& p, const Rgb& q) { return p[c] < q[c]; });
        *it = measure(px, b.begin, mid);
        boxes.push_back(measure(px, mid, b.end));
    }

    std::vector<Rgb> palette;
    for (const Box& b : boxes) {
        unsigned long sum[3] = {0, 0, 0};
        for (std::size_t i = b.begin; i < b.end; ++i)
            for (int c = 0; c < 3; ++c) sum[c] += px[i][c];
        const auto n = static_cast<unsigned long>(b.end - b.begin);
        palette.push_back({static_cast<std::uint8_t>((sum[0] + n / 2) / n), static_cast<std::uint8_t>((sum[1] + n / 2) / n),
                           static_cast<std::uint8_t>((sum[2] + n / 2) / n)});
    }
    return palette;
}

std::vector<std::uint8_t> lzw_encode(const std::vector<std::uint8_t>& indices, int min_code_size) {
    if (min_code_size < 2 || min_code_size > 8) throw std::invalid_argument("lzw_encode: min_code_size 2..8");
    const int clear = 1 << min_code_size, eoi = clear + 1;
    for (std::uint8_t i : indices) {
        if (i >= clear) throw std::invalid_argument("lzw_encode: index exceeds the code size");
    }

    std::vector<std::uint8_t> out;
    std::uint32_t buffer = 0;
    int bits = 0;
    const auto emit = [&](int code, int size) {
        buffer |= static_cast<std::uint32_t>(code) << bits;
        bits += size;
        while (bits >= 8) {
            out.push_back(static_cast<std::uint8_t>(buffer & 0xff));
            buffer >>= 8;
            bits -= 8;
        }
    };

    // Dictionary: child code of (prefix code, next index); a generation counter makes "clear"
    // O(1) instead of wiping 4096 x 256 entries.
    struct Entry {
        std::uint16_t code = 0, generation = 0;
    };
    std::vector<Entry> dict(4096u * 256u);
    std::uint16_t generation = 1;
    int code_size = min_code_size + 1;
    int max_code = eoi;

    emit(clear, code_size);
    if (!indices.empty()) {
        int current = indices[0];
        for (std::size_t i = 1; i < indices.size(); ++i) {
            const int next = indices[i];
            Entry& e = dict[static_cast<std::size_t>(current) * 256u + static_cast<std::size_t>(next)];
            if (e.generation == generation) {
                current = e.code;
                continue;
            }
            emit(current, code_size);
            e.code = static_cast<std::uint16_t>(++max_code);
            e.generation = generation;
            if (max_code >= (1 << code_size)) ++code_size;
            if (max_code == 4095) {  // dictionary full: start over
                emit(clear, code_size);
                if (++generation == 0) {
                    std::fill(dict.begin(), dict.end(), Entry{});
                    generation = 1;
                }
                code_size = min_code_size + 1;
                max_code = eoi;
            }
            current = next;
        }
        emit(current, code_size);
    }
    emit(clear, code_size);
    emit(eoi, min_code_size + 1);
    if (bits > 0) out.push_back(static_cast<std::uint8_t>(buffer & 0xff));
    return out;
}

GifEncoder::GifEncoder(int width, int height, int delay_cs, int loop, bool dither)
    : width_(width), height_(height), delay_cs_(delay_cs), dither_(dither) {
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) throw std::invalid_argument("GifEncoder: bad size");
    if (delay_cs < 0 || delay_cs > 65535 || loop < 0 || loop > 65535) throw std::invalid_argument("GifEncoder: bad timing");
    const char* header = "GIF89a";
    out_.insert(out_.end(), header, header + 6);
    put16(width);  // logical screen descriptor, no global colour table
    put16(height);
    put(0x00);
    put(0x00);
    put(0x00);
    const std::uint8_t netscape[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01};
    out_.insert(out_.end(), netscape, netscape + sizeof(netscape));
    put16(loop);
    put(0x00);
}

void GifEncoder::add_frame(const std::uint8_t* rgba) {
    if (finished_) throw std::logic_error("GifEncoder: add_frame after finish");
    if (!rgba) throw std::invalid_argument("GifEncoder: null frame");
    const std::size_t n = static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_);
    std::vector<Rgb> palette = median_cut_palette(rgba, n, 256);

    // Nearest palette entry for each 5-bit-per-channel colour cell, computed lazily.
    std::vector<std::int16_t> lut(32 * 32 * 32, -1);
    const auto nearest = [&](int r5, int g5, int b5) {
        std::int16_t& slot = lut[static_cast<std::size_t>((r5 << 10) | (g5 << 5) | b5)];
        if (slot < 0) {
            const int r = r5 * 8 + 4, g = g5 * 8 + 4, b = b5 * 8 + 4;
            int best = 0, best_d = 1 << 30;
            for (std::size_t i = 0; i < palette.size(); ++i) {
                const int dr = r - palette[i][0], dg = g - palette[i][1], db = b - palette[i][2];
                const int dd = 2 * dr * dr + 4 * dg * dg + db * db;  // rough perceptual weights
                if (dd < best_d) {
                    best_d = dd;
                    best = static_cast<int>(i);
                }
            }
            slot = static_cast<std::int16_t>(best);
        }
        return static_cast<std::uint8_t>(slot);
    };

    std::vector<std::uint8_t> indices(n);
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width_ + x;
            const float off = dither_ ? (kBayer[y & 3][x & 3] / 16.f - 0.5f) * 8.f : 0.f;
            int q[3];
            for (int c = 0; c < 3; ++c) {
                const float v = std::clamp(rgba[4 * i + c] + off, 0.f, 255.f);
                q[c] = std::min(31, static_cast<int>(v) >> 3);
            }
            indices[i] = nearest(q[0], q[1], q[2]);
        }
    }
    palette.resize(256, Rgb{0, 0, 0});

    // Graphic control extension: delay, no transparency, disposal "leave in place".
    const std::uint8_t gce[] = {0x21, 0xF9, 0x04, 0x04};
    out_.insert(out_.end(), gce, gce + sizeof(gce));
    put16(delay_cs_);
    put(0x00);
    put(0x00);
    // Image descriptor with a 256-entry local colour table.
    put(0x2C);
    put16(0);
    put16(0);
    put16(width_);
    put16(height_);
    put(0x87);
    for (const Rgb& c : palette) out_.insert(out_.end(), c.begin(), c.end());
    put(0x08);  // LZW minimum code size
    const std::vector<std::uint8_t> data = lzw_encode(indices, 8);
    for (std::size_t i = 0; i < data.size(); i += 255) {
        const std::size_t len = std::min<std::size_t>(255, data.size() - i);
        put(static_cast<std::uint8_t>(len));
        out_.insert(out_.end(), data.begin() + static_cast<std::ptrdiff_t>(i), data.begin() + static_cast<std::ptrdiff_t>(i + len));
    }
    put(0x00);
    ++frames_;
}

std::vector<std::uint8_t> GifEncoder::finish() {
    if (finished_) throw std::logic_error("GifEncoder: finish called twice");
    if (frames_ == 0) throw std::logic_error("GifEncoder: no frames");
    put(0x3B);
    finished_ = true;
    return std::move(out_);
}

}  // namespace relight
