#include "BVH.h"
#include <algorithm>
#include <limits>
#include <fstream>

struct BVHBuildEntry {
    int nodeIdx;
    int start, end;
};

static void computeBounds(const std::vector<Triangle>& tris, const std::vector<int>& triIndices, int start, int end, glm::vec3& bmin, glm::vec3& bmax) {
    bmin = glm::vec3(std::numeric_limits<float>::max());
    bmax = glm::vec3(-std::numeric_limits<float>::max());
    for (int i = start; i < end; ++i) {
        const Triangle& t = tris[triIndices[i]];
        bmin = glm::min(bmin, glm::min(t.v0, glm::min(t.v1, t.v2)));
        bmax = glm::max(bmax, glm::max(t.v0, glm::max(t.v1, t.v2)));
    }
}

// Sweep-based SAH for much faster BVH construction
static int findSAHSplit(const std::vector<Triangle>& tris, const std::vector<int>& triIndices, int start, int end, int& axis, float& splitPos, std::vector<int>& sortedTriIndices) {
    int bestAxis = -1;
    float bestCost = std::numeric_limits<float>::max();
    int bestSplit = -1;
    float bestSplitPos = 0.0f;
    int N = end - start;
    if (N <= 4) return -1;
    // Compute parent bounds for normalization
    glm::vec3 parentBMin, parentBMax;
    computeBounds(tris, triIndices, start, end, parentBMin, parentBMax);
    float parentArea = 2.0f * (
        (parentBMax.x - parentBMin.x) * (parentBMax.y - parentBMin.y) +
        (parentBMax.y - parentBMin.y) * (parentBMax.z - parentBMin.z) +
        (parentBMax.z - parentBMin.z) * (parentBMax.x - parentBMin.x));
    for (int a = 0; a < 3; ++a) {
        // Sort indices by centroid along axis a
        std::vector<std::pair<float, int>> centroidIdxs(N);
        for (int i = 0; i < N; ++i) {
            const Triangle& t = tris[triIndices[start + i]];
            glm::vec3 centroid = (t.v0 + t.v1 + t.v2) / 3.0f;
            centroidIdxs[i] = {centroid[a], triIndices[start + i]};
        }
        std::sort(centroidIdxs.begin(), centroidIdxs.end());
        // Precompute left and right bounds
        std::vector<glm::vec3> leftBMin(N), leftBMax(N), rightBMin(N), rightBMax(N);
        glm::vec3 bmin = glm::vec3(std::numeric_limits<float>::max());
        glm::vec3 bmax = glm::vec3(-std::numeric_limits<float>::max());
        for (int i = 0; i < N; ++i) {
            const Triangle& t = tris[centroidIdxs[i].second];
            bmin = glm::min(bmin, glm::min(t.v0, glm::min(t.v1, t.v2)));
            bmax = glm::max(bmax, glm::max(t.v0, glm::max(t.v1, t.v2)));
            leftBMin[i] = bmin;
            leftBMax[i] = bmax;
        }
        bmin = glm::vec3(std::numeric_limits<float>::max());
        bmax = glm::vec3(-std::numeric_limits<float>::max());
        for (int i = N - 1; i >= 0; --i) {
            const Triangle& t = tris[centroidIdxs[i].second];
            bmin = glm::min(bmin, glm::min(t.v0, glm::min(t.v1, t.v2)));
            bmax = glm::max(bmax, glm::max(t.v0, glm::max(t.v1, t.v2)));
            rightBMin[i] = bmin;
            rightBMax[i] = bmax;
        }
        // Sweep to find best split
        for (int i = 1; i < N; ++i) {
            float leftArea = 2.0f * ((leftBMax[i-1].x - leftBMin[i-1].x) * (leftBMax[i-1].y - leftBMin[i-1].y) +
                                     (leftBMax[i-1].y - leftBMin[i-1].y) * (leftBMax[i-1].z - leftBMin[i-1].z) +
                                     (leftBMax[i-1].z - leftBMin[i-1].z) * (leftBMax[i-1].x - leftBMin[i-1].x));
            float rightArea = 2.0f * ((rightBMax[i].x - rightBMin[i].x) * (rightBMax[i].y - rightBMin[i].y) +
                                      (rightBMax[i].y - rightBMin[i].y) * (rightBMax[i].z - rightBMin[i].z) +
                                      (rightBMax[i].z - rightBMin[i].z) * (rightBMax[i].x - rightBMin[i].x));
            float cost = (leftArea * i + rightArea * (N - i)) / (parentArea + 1e-6f); // normalize
            if (cost < bestCost) {
                bestCost = cost;
                bestAxis = a;
                bestSplit = i;
                bestSplitPos = 0.5f * (centroidIdxs[i-1].first + centroidIdxs[i].first);
            }
        }
    }
    // Output sortedTriIndices for the best axis
    if (bestAxis != -1) {
        std::vector<std::pair<float, int>> centroidIdxs(N);
        for (int i = 0; i < N; ++i) {
            const Triangle& t = tris[triIndices[start + i]];
            glm::vec3 centroid = (t.v0 + t.v1 + t.v2) / 3.0f;
            centroidIdxs[i] = {centroid[bestAxis], triIndices[start + i]};
        }
        std::sort(centroidIdxs.begin(), centroidIdxs.end());
        sortedTriIndices.resize(N);
        for (int i = 0; i < N; ++i) sortedTriIndices[i] = centroidIdxs[i].second;
    }
    axis = bestAxis;
    splitPos = bestSplitPos;
    return bestSplit;
}

void BVH::buildBLAS(const std::vector<Triangle>& tris) {
    triIndices.resize(tris.size());
    for (int i = 0; i < (int)tris.size(); ++i) triIndices[i] = i;
    nodes.clear();
    nodes.reserve(tris.size() * 2);
    std::vector<BVHBuildEntry> stack;
    stack.push_back({0, 0, (int)tris.size()});
    nodes.push_back({glm::vec3(0), 0, glm::vec3(0), 0}); // root
    while (!stack.empty()) {
        BVHBuildEntry e = stack.back(); stack.pop_back();
        int nidx = e.nodeIdx;
        int start = e.start, end = e.end, count = end - start;
        glm::vec3 bmin, bmax;
        computeBounds(tris, triIndices, start, end, bmin, bmax);
        nodes[nidx].boundsMin = bmin;
        nodes[nidx].boundsMax = bmax;
        if (count <= 4) { // leaf
            nodes[nidx].leftFirst = start;
            nodes[nidx].count = count;
            continue;
        }
        int axis = 0;
        float split = 0.0f;
        int mid = start;
        if (splitMethod == BVHSplitMethod::SAH) {
            int sahSplit = -1;
            std::vector<int> sortedTriIndices;
            // Modified findSAHSplit to also return sortedTriIndices
            sahSplit = findSAHSplit(tris, triIndices, start, end, axis, split, sortedTriIndices);
            // Robustness: Only use SAH split if valid, else fallback to midpoint
            if (sahSplit > 0 && sahSplit < (end - start) && sortedTriIndices.size() == (size_t)(end - start)) {
                // Copy sortedTriIndices back into triIndices[start:end]
                for (int i = 0; i < end - start; ++i) {
                    triIndices[start + i] = sortedTriIndices[i];
                }
                mid = start + sahSplit;
            } else {
                // fallback to midpoint
                glm::vec3 extent = bmax - bmin;
                if (extent.y > extent.x && extent.y > extent.z) axis = 1;
                else if (extent.z > extent.x) axis = 2;
                split = 0.5f * (bmin[axis] + bmax[axis]);
                mid = start;
                for (int i = start; i < end; ++i) {
                    glm::vec3 centroid = (tris[triIndices[i]].v0 + tris[triIndices[i]].v1 + tris[triIndices[i]].v2) / 3.0f;
                    if (centroid[axis] < split) {
                        std::swap(triIndices[i], triIndices[mid]);
                        ++mid;
                    }
                }
                if (mid == start || mid == end) mid = start + (count / 2);
            }
        } else {
            glm::vec3 extent = bmax - bmin;
            if (extent.y > extent.x && extent.y > extent.z) axis = 1;
            else if (extent.z > extent.x) axis = 2;
            split = 0.5f * (bmin[axis] + bmax[axis]);
            mid = start;
            for (int i = start; i < end; ++i) {
                glm::vec3 centroid = (tris[triIndices[i]].v0 + tris[triIndices[i]].v1 + tris[triIndices[i]].v2) / 3.0f;
                if (centroid[axis] < split) {
                    std::swap(triIndices[i], triIndices[mid]);
                    ++mid;
                }
            }
            if (mid == start || mid == end) mid = start + (count / 2);
        }
        int leftIdx = (int)nodes.size();
        int rightIdx = leftIdx + 1;
        nodes[nidx].leftFirst = leftIdx;
        nodes[nidx].count = -1;
        nodes.push_back({});
        nodes.push_back({});
        stack.push_back({rightIdx, mid, end});
        stack.push_back({leftIdx, start, mid});
    }
}

// TLAS: build over mesh AABBs, leaves reference BVHInstance indices
void BVH::buildTLAS(const std::vector<BVHInstance>& meshInstances, const std::vector<BVHNode>& meshRootNodes) {
    triIndices.clear();
    nodes.clear();
    int numMeshes = (int)meshInstances.size();
    // Each mesh's root node AABB
    std::vector<int> meshIndices(numMeshes);
    for (int i = 0; i < numMeshes; ++i) meshIndices[i] = i;
    // Stack for iterative build
    std::vector<BVHBuildEntry> stack;
    stack.push_back({0, 0, numMeshes});
    nodes.push_back({glm::vec3(0), 0, glm::vec3(0), 0}); // root
    while (!stack.empty()) {
        BVHBuildEntry e = stack.back(); stack.pop_back();
        int nidx = e.nodeIdx;
        int start = e.start, end = e.end, count = end - start;
        glm::vec3 bmin, bmax;
        // Compute bounds over mesh root nodes
        bmin = glm::vec3(std::numeric_limits<float>::max());
        bmax = glm::vec3(-std::numeric_limits<float>::max());
        for (int i = start; i < end; ++i) {
            const BVHNode& meshRoot = meshRootNodes[meshIndices[i]];
            bmin = glm::min(bmin, meshRoot.boundsMin);
            bmax = glm::max(bmax, meshRoot.boundsMax);
        }
        nodes[nidx].boundsMin = bmin;
        nodes[nidx].boundsMax = bmax;
        if (count == 1) { // leaf: single instance
            nodes[nidx].leftFirst = (int)triIndices.size();
            nodes[nidx].count = 1;
            triIndices.push_back(meshIndices[start]);
            continue;
        }
        //if (count == 2) { // leaf: two instances
        //    nodes[nidx].leftFirst = (int)triIndices.size();
        //    nodes[nidx].count = 2;
        //    triIndices.push_back(meshIndices[start]);
        //    triIndices.push_back(meshIndices[start+1]);
        //    continue;
        //}
        glm::vec3 extent = bmax - bmin;
        int axis = 0;
        if (extent.y > extent.x && extent.y > extent.z) axis = 1;
        else if (extent.z > extent.x) axis = 2;
        float split = 0.5f * (bmin[axis] + bmax[axis]);
        int mid = start;
        for (int i = start; i < end; ++i) {
            glm::vec3 centroid = (meshRootNodes[meshIndices[i]].boundsMin + meshRootNodes[meshIndices[i]].boundsMax) * 0.5f;
            if (centroid[axis] < split) {
                std::swap(meshIndices[i], meshIndices[mid]);
                ++mid;
            }
        }
        if (mid == start || mid == end) mid = start + (count / 2);
        int leftIdx = (int)nodes.size();
        int rightIdx = leftIdx + 1;
        nodes[nidx].leftFirst = leftIdx;
        nodes[nidx].count = -1;
        nodes.push_back({});
        nodes.push_back({});
        stack.push_back({rightIdx, mid, end});
        stack.push_back({leftIdx, start, mid});
    }
}

bool BVH::saveToFile(const std::string& filename) const {
    std::ofstream out(filename, std::ios::binary);
    if (!out) return false;
    size_t nodeCount = nodes.size();
    size_t triIdxCount = triIndices.size();
    out.write(reinterpret_cast<const char*>(&nodeCount), sizeof(size_t));
    out.write(reinterpret_cast<const char*>(nodes.data()), nodeCount * sizeof(BVHNode));
    out.write(reinterpret_cast<const char*>(&triIdxCount), sizeof(size_t));
    out.write(reinterpret_cast<const char*>(triIndices.data()), triIdxCount * sizeof(int));
    return out.good();
}

bool BVH::loadFromFile(const std::string& filename) {
    std::ifstream in(filename, std::ios::binary);
    if (!in) return false;
    size_t nodeCount = 0, triIdxCount = 0;
    in.read(reinterpret_cast<char*>(&nodeCount), sizeof(size_t));
    nodes.resize(nodeCount);
    in.read(reinterpret_cast<char*>(nodes.data()), nodeCount * sizeof(BVHNode));
    in.read(reinterpret_cast<char*>(&triIdxCount), sizeof(size_t));
    triIndices.resize(triIdxCount);
    in.read(reinterpret_cast<char*>(triIndices.data()), triIdxCount * sizeof(int));
    return in.good();
}
