#include <gtest/gtest.h>

#include <random>
#include <string>
#include <vector>

#include "relight/gif.hpp"

using namespace relight;

namespace {

// Independent GIF LZW decoder (straight from the GIF89a specification), used to verify the
// encoder by round trip.
std::vector<std::uint8_t> lzw_decode(const std::vector<std::uint8_t>& data, int min_code_size) {
    const int clear = 1 << min_code_size, eoi = clear + 1;
    std::vector<std::vector<std::uint8_t>> dict;
    const auto reset = [&] {
        dict.assign(static_cast<std::size_t>(clear) + 2, {});
        for (int i = 0; i < clear; ++i) dict[static_cast<std::size_t>(i)] = {static_cast<std::uint8_t>(i)};
    };
    reset();
    int code_size = min_code_size + 1, prev = -1;
    std::size_t bitpos = 0;
    std::vector<std::uint8_t> out;
    for (;;) {
        if (bitpos + static_cast<std::size_t>(code_size) > data.size() * 8) throw std::runtime_error("truncated");
        int code = 0;
        for (int b = 0; b < code_size; ++b, ++bitpos)
            code |= ((data[bitpos / 8] >> (bitpos % 8)) & 1) << b;
        if (code == clear) {
            reset();
            code_size = min_code_size + 1;
            prev = -1;
            continue;
        }
        if (code == eoi) break;
        std::vector<std::uint8_t> entry;
        if (prev < 0) {
            entry = dict.at(static_cast<std::size_t>(code));
        } else {
            if (code < static_cast<int>(dict.size())) {
                entry = dict[static_cast<std::size_t>(code)];
            } else if (code == static_cast<int>(dict.size())) {
                entry = dict[static_cast<std::size_t>(prev)];
                entry.push_back(entry[0]);
            } else {
                throw std::runtime_error("bad code");
            }
            if (dict.size() < 4096) {
                auto added = dict[static_cast<std::size_t>(prev)];
                added.push_back(entry[0]);
                dict.push_back(added);
            }
        }
        out.insert(out.end(), entry.begin(), entry.end());
        prev = code;
        if (static_cast<int>(dict.size()) == (1 << code_size) && code_size < 12) ++code_size;
    }
    return out;
}

}  // namespace

TEST(Lzw, RoundTripRandomAndRepetitive) {
    std::mt19937 rng(1);
    for (int trial = 0; trial < 6; ++trial) {
        const int alphabet = trial < 3 ? 256 : 4;          // random noise vs long repeated runs
        std::uniform_int_distribution<int> D(0, alphabet - 1);
        std::vector<std::uint8_t> in(50000 + trial * 37);
        for (auto& v : in) v = static_cast<std::uint8_t>(D(rng));
        EXPECT_EQ(lzw_decode(lzw_encode(in, 8), 8), in) << "trial " << trial;
    }
    std::vector<std::uint8_t> zeros(300000, 0);            // forces many dictionary resets
    EXPECT_EQ(lzw_decode(lzw_encode(zeros, 8), 8), zeros);
    EXPECT_EQ(lzw_decode(lzw_encode({}, 8), 8), std::vector<std::uint8_t>{});
}

TEST(Palette, ExactForFewColoursAndBoundedOtherwise) {
    std::vector<std::uint8_t> rgba;
    const Rgb colours[3] = {{255, 0, 0}, {0, 255, 0}, {10, 20, 30}};
    for (int i = 0; i < 300; ++i) {
        const Rgb& c = colours[i % 3];
        rgba.insert(rgba.end(), {c[0], c[1], c[2], 255});
    }
    auto p = median_cut_palette(rgba.data(), 300, 256);
    EXPECT_EQ(p.size(), 3u);

    std::mt19937 rng(2);
    std::uniform_int_distribution<int> D(0, 255);
    std::vector<std::uint8_t> noise(4 * 40000);
    for (auto& v : noise) v = static_cast<std::uint8_t>(D(rng));
    p = median_cut_palette(noise.data(), 40000, 256);
    EXPECT_LE(p.size(), 256u);
    EXPECT_GE(p.size(), 200u);
}

TEST(Gif, FileStructure) {
    const int w = 40, h = 30;
    GifEncoder enc(w, h, 5);
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(w) * h * 4);
    for (int f = 0; f < 3; ++f) {
        for (int i = 0; i < w * h; ++i) {
            frame[4 * i] = static_cast<std::uint8_t>(i % 256);
            frame[4 * i + 1] = static_cast<std::uint8_t>(f * 80);
            frame[4 * i + 2] = 128;
            frame[4 * i + 3] = 255;
        }
        enc.add_frame(frame.data());
    }
    const auto gif = enc.finish();
    EXPECT_EQ(std::string(gif.begin(), gif.begin() + 6), "GIF89a");
    EXPECT_EQ(gif[6] | (gif[7] << 8), w);
    EXPECT_EQ(gif[8] | (gif[9] << 8), h);
    EXPECT_EQ(gif.back(), 0x3B);
    // Three image descriptors, each preceded by a graphic control extension with the delay.
    int frames = 0;
    for (std::size_t i = 0; i + 7 < gif.size(); ++i) {
        if (gif[i] == 0x21 && gif[i + 1] == 0xF9 && gif[i + 2] == 0x04 && gif[i + 4] == 5 && gif[i + 5] == 0 &&
            gif[i + 8] == 0x2C) {
            ++frames;
        }
    }
    EXPECT_EQ(frames, 3);
    EXPECT_THROW(enc.add_frame(frame.data()), std::logic_error);
}

TEST(Gif, FrameDecodesToPaletteOfTheSource) {
    // 4 flat colours, no dithering: decoding the first frame must give back exactly them.
    const int w = 64, h = 16;
    std::vector<std::uint8_t> frame(static_cast<std::size_t>(w) * h * 4);
    const Rgb colours[4] = {{0, 0, 0}, {255, 255, 255}, {200, 40, 40}, {40, 40, 200}};
    for (int i = 0; i < w * h; ++i) {
        const Rgb& c = colours[(i % w) / 16];
        frame[4 * i] = c[0];
        frame[4 * i + 1] = c[1];
        frame[4 * i + 2] = c[2];
        frame[4 * i + 3] = 255;
    }
    GifEncoder enc(w, h, 4, 0, /*dither=*/false);
    enc.add_frame(frame.data());
    const auto gif = enc.finish();

    // Header 13 + NETSCAPE 19 + GCE 8 = 40; descriptor 10; table 768; min code size 1.
    std::size_t p = 40;
    ASSERT_EQ(gif[p], 0x2C);
    p += 10;
    const std::size_t table = p;
    p += 768;
    ASSERT_EQ(gif[p++], 8);
    std::vector<std::uint8_t> lzw;
    while (gif[p] != 0) {
        const std::size_t len = gif[p++];
        lzw.insert(lzw.end(), gif.begin() + static_cast<std::ptrdiff_t>(p), gif.begin() + static_cast<std::ptrdiff_t>(p + len));
        p += len;
    }
    const auto idx = lzw_decode(lzw, 8);
    ASSERT_EQ(idx.size(), static_cast<std::size_t>(w * h));
    for (int i = 0; i < w * h; ++i) {
        const Rgb& c = colours[(i % w) / 16];
        for (int ch = 0; ch < 3; ++ch) EXPECT_EQ(gif[table + 3 * idx[static_cast<std::size_t>(i)] + ch], c[ch]) << i;
    }
}
