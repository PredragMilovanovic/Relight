// Animated GIF encoder (GIF89a): median-cut palette per frame, ordered dithering, LZW.
// Used by "Save animation" in the app and to produce the README animation.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace relight {

using Rgb = std::array<std::uint8_t, 3>;

// Median-cut colour quantization of RGBA pixels (alpha ignored) to at most max_colors colours.
// If the image has <= max_colors distinct colours they are returned exactly.
std::vector<Rgb> median_cut_palette(const std::uint8_t* rgba, std::size_t pixel_count, int max_colors);

// GIF-flavoured LZW (variable code size, clear code first, EOI last), packed LSB-first.
// Returns the raw bit stream, not yet split into sub-blocks.
std::vector<std::uint8_t> lzw_encode(const std::vector<std::uint8_t>& indices, int min_code_size);

class GifEncoder {
public:
    // delay in 1/100 s; loop = 0 repeats forever.
    GifEncoder(int width, int height, int delay_cs, int loop = 0, bool dither = true);

    void add_frame(const std::uint8_t* rgba);   // width * height * 4 bytes
    std::vector<std::uint8_t> finish();         // appends the trailer; the encoder is then closed

    int frames() const { return frames_; }
    int width() const { return width_; }
    int height() const { return height_; }

private:
    void put(std::uint8_t b) { out_.push_back(b); }
    void put16(int v) { put(static_cast<std::uint8_t>(v & 0xff)); put(static_cast<std::uint8_t>((v >> 8) & 0xff)); }

    int width_, height_, delay_cs_;
    bool dither_;
    int frames_ = 0;
    bool finished_ = false;
    std::vector<std::uint8_t> out_;
};

}  // namespace relight
