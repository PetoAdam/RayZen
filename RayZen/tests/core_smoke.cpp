#include <iostream>
#include <string>

#include <glm/glm.hpp>

#include "BVH.h"
#include "Mesh.h"

namespace {

bool boundsContain(const BVHNode& node, const glm::vec3& point) {
    const float epsilon = 1e-4f;
    return point.x >= node.boundsMin.x - epsilon && point.x <= node.boundsMax.x + epsilon &&
           point.y >= node.boundsMin.y - epsilon && point.y <= node.boundsMax.y + epsilon &&
           point.z >= node.boundsMin.z - epsilon && point.z <= node.boundsMax.z + epsilon;
}

int fail(const std::string& message) {
    std::cerr << message << std::endl;
    return 1;
}

} // namespace

int main() {
    Mesh cube;
    if (!cube.loadFromOBJ(std::string(RAYZEN_SOURCE_DIR) + "/meshes/cube.obj", 0)) {
        return fail("Failed to load cube.obj");
    }
    if (cube.triangles.size() != 12) {
        return fail("cube.obj triangle count changed unexpectedly");
    }

    Mesh monkey;
    if (!monkey.loadFromOBJ(std::string(RAYZEN_SOURCE_DIR) + "/meshes/monkey.obj", 1)) {
        return fail("Failed to load monkey.obj");
    }
    if (monkey.triangles.size() != 968) {
        return fail("monkey.obj triangle count changed unexpectedly");
    }

    BVH monkeyBvh;
    monkeyBvh.buildBLAS(monkey.triangles);
    if (monkeyBvh.nodes.empty() || monkeyBvh.triIndices.size() != monkey.triangles.size()) {
        return fail("BLAS build did not generate the expected node/index data");
    }

    const BVHNode& root = monkeyBvh.nodes.front();
    for (const Triangle& triangle : monkey.triangles) {
        if (!boundsContain(root, triangle.v0) || !boundsContain(root, triangle.v1) || !boundsContain(root, triangle.v2)) {
            return fail("BLAS root bounds do not enclose the monkey mesh");
        }
    }

    return 0;
}