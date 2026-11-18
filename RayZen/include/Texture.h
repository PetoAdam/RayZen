#ifndef RAYZEN_TEXTURE_H
#define RAYZEN_TEXTURE_H

#include <cstdint>
#include <string>
#include <vector>

struct TextureAsset {
    bool loadPNG(const std::string& path);
    bool setRGBA(std::string name, int width, int height, std::vector<std::uint8_t> rgba);
    static TextureAsset checker();

    const std::string& name() const noexcept { return name_; }
    const std::string& sourcePath() const noexcept { return sourcePath_; }
    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    const std::vector<std::uint8_t>& rgba() const noexcept { return rgba_; }
    std::uint64_t revision() const noexcept { return revision_; }

private:
    std::string name_;
    std::string sourcePath_;
    int width_ = 0;
    int height_ = 0;
    std::vector<std::uint8_t> rgba_;
    std::uint64_t revision_ = 0;
};

#endif
