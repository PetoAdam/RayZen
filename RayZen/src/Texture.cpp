#include "Texture.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <png.h>

#include "Logger.h"

namespace {
std::atomic<std::uint64_t> nextRevision{1};
}

bool TextureAsset::setRGBA(std::string name, int width, int height, std::vector<std::uint8_t> rgba) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 ||
        rgba.size() != static_cast<size_t>(width) * static_cast<size_t>(height) * 4) {
        Logger::error("Invalid texture dimensions or pixel data: " + name);
        return false;
    }
    name_ = std::move(name);
    sourcePath_.clear();
    width_ = width;
    height_ = height;
    rgba_ = std::move(rgba);
    revision_ = nextRevision.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool TextureAsset::loadPNG(const std::string& path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    if (ec) {
        Logger::error("Cannot resolve texture path '" + path + "': " + ec.message());
        return false;
    }
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&image, path.c_str())) {
        Logger::error("Cannot read texture '" + path + "': " + image.message);
        png_image_free(&image);
        return false;
    }
    image.format = PNG_FORMAT_RGBA;
    if (image.width == 0 || image.height == 0 || image.width > 4096 || image.height > 4096) {
        Logger::error("Texture dimensions must be between 1 and 4096: " + path);
        png_image_free(&image);
        return false;
    }
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr)) {
        Logger::error("Cannot decode texture '" + path + "': " + image.message);
        png_image_free(&image);
        return false;
    }
    const size_t rowSize = static_cast<size_t>(image.width) * 4;
    for (size_t y = 0; y < image.height / 2; ++y)
        std::swap_ranges(pixels.begin() + y * rowSize, pixels.begin() + (y + 1) * rowSize,
                         pixels.begin() + (image.height - y - 1) * rowSize);
    const int width = static_cast<int>(image.width);
    const int height = static_cast<int>(image.height);
    png_image_free(&image);
    if (!setRGBA(path, width, height, std::move(pixels))) return false;
    sourcePath_ = absolute.lexically_normal().string();
    return true;
}

TextureAsset TextureAsset::checker() {
    TextureAsset texture;
    std::vector<std::uint8_t> pixels(32 * 32 * 4);
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const std::uint8_t value = ((x / 2 + y / 2) % 2) ? 100 : 255;
            const size_t offset = static_cast<size_t>(y * 32 + x) * 4;
            pixels[offset] = pixels[offset + 1] = pixels[offset + 2] = value;
            pixels[offset + 3] = 255;
        }
    }
    texture.setRGBA("CHECKER", 32, 32, std::move(pixels));
    return texture;
}
