// Minimal owning image container. Row 0 is the TOP row of the photo (the convention of image
// files and canvas ImageData); texture coordinates (u, v) have v = 1 at the top.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace relight {

template <typename T>
struct Image {
    int width = 0;
    int height = 0;
    int channels = 1;
    std::vector<T> data;

    Image() = default;

    Image(int w, int h, int c = 1, T fill = T{}) : width(w), height(h), channels(c) {
        if (w <= 0 || h <= 0 || c <= 0) {
            throw std::invalid_argument("Image: width, height and channels must be positive");
        }
        data.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * static_cast<std::size_t>(c), fill);
    }

    std::size_t pixels() const { return static_cast<std::size_t>(width) * static_cast<std::size_t>(height); }

    std::size_t index(int x, int y, int c = 0) const {
        return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) *
                   static_cast<std::size_t>(channels) +
               static_cast<std::size_t>(c);
    }

    T& at(int x, int y, int c = 0) { return data[index(x, y, c)]; }
    const T& at(int x, int y, int c = 0) const { return data[index(x, y, c)]; }
};

using ImageF = Image<float>;
using ImageU8 = Image<std::uint8_t>;

}  // namespace relight
