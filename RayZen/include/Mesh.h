#ifndef MESH_H
#define MESH_H

#include <vector>
#include <string>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// A single triangle with three vertices and a material index.
struct Triangle {
    glm::vec3 v0;
    float pad0 = 0.0f; // Padding to align to 16 bytes
    glm::vec3 v1;
    float pad1 = 0.0f; // Padding to align to 16 bytes
    glm::vec3 v2;
    float pad2 = 0.0f; // Padding to align to 16 bytes
    int materialIndex;
} __attribute__((aligned(16)));

class Mesh {
public:
    std::vector<Triangle> triangles;
    glm::mat4 transform = glm::mat4(1.0f);  // Default: no transformation

    // Loads a simple OBJ file (only vertices and faces, no textures/normals)
    // and assigns all triangles the given material index.
    bool loadFromOBJ(const std::string& filename, int materialIndex);
};

#endif
