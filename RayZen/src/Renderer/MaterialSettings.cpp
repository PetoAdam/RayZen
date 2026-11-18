#include "Renderer/MaterialSettings.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>

#include "Logger.h"

namespace rayzen {
namespace {

bool valid(const Material& m) {
    for (int c = 0; c < 3; ++c)
        if (!std::isfinite(m.albedo[c]) || m.albedo[c] < 0.0f || m.albedo[c] > 1.0f) return false;
    for (float v : {m.metallic, m.roughness, m.reflectivity, m.transparency})
        if (!std::isfinite(v) || v < 0.0f || v > 1.0f) return false;
    return std::isfinite(m.ior) && m.ior > 0.0f && std::isfinite(m.subsurfaceMeanFreePath) &&
           m.subsurfaceMeanFreePath >= 0.0f;
}

}

bool loadMaterialSettings(const std::filesystem::path& path, Scene& scene) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        Logger::error("Cannot inspect material settings: " + ec.message());
        return false;
    }
    if (!exists) return true;
    std::ifstream input(path);
    if (!input) {
        Logger::error("Cannot open material settings: " + path.string());
        return false;
    }
    auto materials = scene.materials;
    auto textures = scene.textures;
    std::vector<bool> seen(materials.size(), false);
    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line)) {
        ++lineNumber;
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        std::istringstream in(line);
        size_t index = 0;
        glm::vec3 color;
        float metallic, roughness, reflectivity, transparency, ior, distance;
        std::string texture, extra;
        if (!(in >> index >> color.r >> color.g >> color.b >> metallic >> roughness >>
              reflectivity >> transparency >> ior >> distance >> std::quoted(texture)) ||
            (in >> extra) || index >= materials.size() || seen[index]) {
            Logger::error("Invalid material settings at line " + std::to_string(lineNumber));
            return false;
        }
        Material next(color, metallic, roughness, reflectivity, transparency, ior, distance);
        if (!valid(next)) {
            Logger::error("Out-of-range material settings at line " + std::to_string(lineNumber));
            return false;
        }
        if (texture == "@checker") {
            if (textures.empty() || textures[0].name() != "CHECKER" || !textures[0].sourcePath().empty()) {
                Logger::error("Built-in checker texture is unavailable");
                return false;
            }
            next.baseColorTexture = 0;
        } else if (texture != "@none") {
            if (texture.compare(0, 5, "file:") != 0 || texture.size() == 5) {
                Logger::error("Invalid material texture reference at line " + std::to_string(lineNumber));
                return false;
            }
            const std::string filename = texture.substr(5);
            if (!std::filesystem::path(filename).is_absolute()) {
                Logger::error("Material texture paths must be absolute at line " + std::to_string(lineNumber));
                return false;
            }
            size_t textureIndex = 0;
            for (; textureIndex < textures.size(); ++textureIndex)
                if (textures[textureIndex].sourcePath() == filename) break;
            if (textureIndex == textures.size()) {
                TextureAsset asset;
                if (!asset.loadPNG(filename)) return false;
                textures.push_back(std::move(asset));
                Logger::info("Material texture " + std::to_string(textureIndex) + ": " + filename);
            }
            next.baseColorTexture = static_cast<int>(textureIndex);
        }
        materials[index] = next;
        seen[index] = true;
    }
    if (!input.eof()) {
        Logger::error("Failed while reading material settings: " + path.string());
        return false;
    }
    scene.materials = std::move(materials);
    scene.textures = std::move(textures);
    Logger::info("Loaded material settings from " + path.string());
    return true;
}

bool saveMaterialSettings(const std::filesystem::path& path, const Scene& scene) {
    std::ostringstream data;
    data << "# RayZen material settings: index RGB metallic roughness reflectivity transparency IOR SSS texture\n";
    data << std::setprecision(std::numeric_limits<float>::max_digits10);
    for (size_t i = 0; i < scene.materials.size(); ++i) {
        const Material& m = scene.materials[i];
        if (!valid(m) || m.baseColorTexture < -1 ||
            (m.baseColorTexture >= 0 && static_cast<size_t>(m.baseColorTexture) >= scene.textures.size())) {
            Logger::error("Cannot save invalid material " + std::to_string(i));
            return false;
        }
        std::string texture = "@none";
        if (m.baseColorTexture >= 0) {
            const TextureAsset& asset = scene.textures[m.baseColorTexture];
            if (m.baseColorTexture == 0 && asset.name() == "CHECKER" && asset.sourcePath().empty()) {
                texture = "@checker";
            } else if (!asset.sourcePath().empty()) {
                if (asset.sourcePath().find_first_of("\r\n") != std::string::npos) {
                    Logger::error("Cannot persist a texture path containing a newline");
                    return false;
                }
                texture = "file:" + asset.sourcePath();
            } else {
                Logger::error("Cannot persist material texture without a PNG source: " + asset.name());
                return false;
            }
        }
        data << i << ' ' << m.albedo.r << ' ' << m.albedo.g << ' ' << m.albedo.b << ' '
             << m.metallic << ' ' << m.roughness << ' ' << m.reflectivity << ' '
             << m.transparency << ' ' << m.ior << ' ' << m.subsurfaceMeanFreePath << ' '
             << std::quoted(texture) << '\n';
    }
    const auto tmp = path.string() + ".tmp-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream out(tmp, std::ios::binary);
    out << data.str();
    out.close();
    if (!out) {
        Logger::error("Failed to write material settings: " + tmp);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        Logger::error("Cannot save material settings: " + ec.message());
        std::error_code cleanupError;
        std::filesystem::remove(tmp, cleanupError);
        return false;
    }
    Logger::info("Saved material settings to " + path.string());
    return true;
}

} // namespace rayzen
