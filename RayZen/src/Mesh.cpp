#include "Mesh.h"
#include "Logger.h"
#include <charconv>
#include <fstream>
#include <sstream>

namespace {
bool objIndex(const std::string& text, size_t count, size_t& index) {
    int value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0) return false;
    const long long resolved = value > 0 ? static_cast<long long>(value) - 1 :
                               static_cast<long long>(count) + value;
    if (resolved < 0 || resolved >= static_cast<long long>(count)) return false;
    index = static_cast<size_t>(resolved);
    return true;
}
}

bool Mesh::loadFromOBJ(const std::string& filename, int materialIndex) {
    triangles.clear();

    std::ifstream file(filename);
    if (!file.is_open()) {
        Logger::error("Failed to open OBJ file: " + filename);
        return false;
    }
    
    std::vector<glm::vec3> vertices;
    std::vector<glm::vec2> texcoords;
    std::string line;
    while (std::getline(file, line)) {
        // Parse vertex lines (e.g., "v 0.0 1.0 0.0")
        if (line.substr(0, 2) == "v ") {
            std::istringstream iss(line.substr(2));
            glm::vec3 v;
            iss >> v.x >> v.y >> v.z;
            vertices.push_back(v);
        }
        else if (line.substr(0, 3) == "vt ") {
            std::istringstream iss(line.substr(3));
            glm::vec2 uv;
            if (!(iss >> uv.x >> uv.y)) {
                Logger::error("Invalid OBJ texture coordinate in: " + filename);
                return false;
            }
            texcoords.push_back(uv);
        }
        else if (line.substr(0, 2) == "f ") {
            std::istringstream iss(line.substr(2));
            struct Corner { size_t vertex; size_t uv; bool hasUV; };
            std::vector<Corner> corners;
            std::string token;
            while (iss >> token) {
                if (!token.empty() && token[0] == '#') break;
                const size_t first = token.find('/');
                const size_t second = first == std::string::npos ? std::string::npos : token.find('/', first + 1);
                Corner corner{};
                if (!objIndex(token.substr(0, first), vertices.size(), corner.vertex)) {
                    Logger::error("OBJ face index out of range in: " + filename);
                    return false;
                }
                const std::string uv = first == std::string::npos ? "" :
                    token.substr(first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
                corner.hasUV = !uv.empty();
                if (corner.hasUV && !objIndex(uv, texcoords.size(), corner.uv)) {
                    Logger::error("OBJ texture coordinate index out of range in: " + filename);
                    return false;
                }
                if (!corners.empty() && corner.hasUV != corners.front().hasUV) {
                    Logger::error("OBJ face mixes vertices with and without texture coordinates in: " + filename);
                    return false;
                }
                corners.push_back(corner);
            }
            if (corners.size() >= 3) {
                for (size_t i = 1; i < corners.size() - 1; ++i) {
                    Triangle tri{};
                    tri.v0 = vertices[corners[0].vertex];
                    tri.v1 = vertices[corners[i].vertex];
                    tri.v2 = vertices[corners[i + 1].vertex];
                    tri.materialIndex = materialIndex;
                    tri.hasUV = corners[0].hasUV ? 1 : 0;
                    if (tri.hasUV) {
                        tri.uv0 = texcoords[corners[0].uv];
                        tri.uv1 = texcoords[corners[i].uv];
                        tri.uv2 = texcoords[corners[i + 1].uv];
                    }
                    triangles.push_back(tri);
                }
            }
        }
    }
    Logger::debug("Loaded " + std::to_string(triangles.size()) + " triangles.");
    return true;
}
