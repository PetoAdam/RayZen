// Bounding Volume Hierarchy
#pragma once
#include <vector>
#include <glm/glm.hpp>
#include "Mesh.h"

struct BVHNode {
    glm::vec3 boundsMin;
    int leftFirst; // left child index or first triangle index
    glm::vec3 boundsMax;
    int count;     // if leaf: number of triangles, else: -1
};

class BVH {
public:
    std::vector<BVHNode> nodes;
    std::vector<int> triIndices;
    void build(const std::vector<Triangle>& tris);
};
