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

struct BVHInstance {
    int blasNodeOffset; // Offset into global BLAS node buffer
    int blasTriOffset; // Offset into global BLAS triangle index buffer
    int meshIndex;     // Index of the mesh in the scene
    int globalTriOffset; // Offset into global triangle buffer (NEW)
    glm::mat4 transform; // (optional) for instancing, identity if unused
};

enum class BVHSplitMethod {
    Midpoint,
    SAH
};

class BVH {
public:
    std::vector<BVHNode> nodes;
    std::vector<int> triIndices;
    // For TLAS
    std::vector<BVHInstance> instances;
    BVHSplitMethod splitMethod = BVHSplitMethod::SAH;
    // BLAS build (per mesh)
    void buildBLAS(const std::vector<Triangle>& tris);
    // TLAS build (over mesh AABBs)
    void buildTLAS(const std::vector<BVHInstance>& meshInstances, const std::vector<BVHNode>& meshRootNodes);
    // BVH serialization
    bool saveToFile(const std::string& filename) const;
    bool loadFromFile(const std::string& filename);
};
