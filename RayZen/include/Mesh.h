#ifndef MESH_H
#define MESH_H

#include <vector>
#include <string>
#include <cstddef>
#include <glm/glm.hpp>

// A single triangle with three vertices and a material index.
struct alignas(16) Triangle {
    glm::vec3 v0;
    float pad0 = 0.0f; // Padding to align to 16 bytes
    glm::vec3 v1;
    float pad1 = 0.0f; // Padding to align to 16 bytes
    glm::vec3 v2;
    float pad2 = 0.0f; // Padding to align to 16 bytes
    int materialIndex = 0;
    int hasUV = 0;
    glm::vec2 uv0{0.0f};
    glm::vec2 uv1{0.0f};
    glm::vec2 uv2{0.0f};
};

static_assert(offsetof(Triangle, uv0) == 56 && sizeof(Triangle) == 80,
              "Triangle must match the GLSL std430 layout");

class Mesh {
public:
    std::vector<Triangle> triangles;
    bool loadFromOBJ(const std::string& filename, int materialIndex);
};

#endif
