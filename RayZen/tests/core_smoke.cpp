#include <iostream>
#include <cmath>
#include <string>
#include <chrono>
#include <filesystem>
#include <fstream>

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
    if (!cube.triangles[0].hasUV || cube.triangles[0].uv0 != glm::vec2(1.0f, 0.333333f))
        return fail("OBJ texture coordinates were not preserved");

    const auto uvPath = std::filesystem::temp_directory_path() /
        ("rayzen-uv-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".obj");
    {
        std::ofstream obj(uvPath);
        obj << "v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0.1 0.2\nvt 0.8 0.2\nvt 0.1 0.9\n"
               "f -3/-3 -2/-2 -1/-1\n";
    }
    Mesh uvMesh;
    const bool loadedUV = uvMesh.loadFromOBJ(uvPath.string(), 0);
    std::filesystem::remove(uvPath);
    if (!loadedUV || uvMesh.triangles.size() != 1 || !uvMesh.triangles[0].hasUV ||
        uvMesh.triangles[0].uv1 != glm::vec2(0.8f, 0.2f))
        return fail("OBJ negative texture-coordinate indices were not resolved");

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

    std::vector<BVHInstance> instances(80);
    std::vector<BVHNode> roots(80);
    for (size_t i = 0; i < roots.size(); ++i) {
        const float x = std::ldexp(1.0f, static_cast<int>(i));
        roots[i].boundsMin = roots[i].boundsMax = glm::vec3(x, 0, 0);
    }
    BVH tlas;
    tlas.buildTLAS(instances, roots);
    std::vector<std::pair<int, int>> stack{{0, 0}};
    size_t leafInstances = 0;
    while (!stack.empty()) {
        const auto [index, depth] = stack.back();
        stack.pop_back();
        if (depth > BVH::MaxDepth) return fail("TLAS exceeds the shader traversal depth");
        const BVHNode& node = tlas.nodes[index];
        if (node.count > 0) {
            leafInstances += static_cast<size_t>(node.count);
        } else {
            stack.emplace_back(node.leftFirst, depth + 1);
            stack.emplace_back(node.leftFirst + 1, depth + 1);
        }
    }
    if (leafInstances != instances.size() || tlas.triIndices.size() != instances.size())
        return fail("Depth-limited TLAS lost instances");
    BVH empty;
    empty.buildTLAS({}, {});
    empty.buildBLAS({});
    if (!empty.nodes.empty()) return fail("Empty BVH should contain no root node");

    return 0;
}