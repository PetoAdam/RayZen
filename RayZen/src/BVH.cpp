#include "BVH.h"
#include <algorithm>
#include <limits>

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

void BVH::build(const std::vector<Triangle>& tris) {
    triIndices.resize(tris.size());
    for (int i = 0; i < (int)tris.size(); ++i) triIndices[i] = i;
    nodes.clear();
    nodes.reserve(tris.size() * 2);
    // Stack for iterative build
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
        // Find longest axis
        glm::vec3 extent = bmax - bmin;
        int axis = 0;
        if (extent.y > extent.x && extent.y > extent.z) axis = 1;
        else if (extent.z > extent.x) axis = 2;
        // Partition
        float split = 0.5f * (bmin[axis] + bmax[axis]);
        int mid = start;
        for (int i = start; i < end; ++i) {
            glm::vec3 centroid = (tris[triIndices[i]].v0 + tris[triIndices[i]].v1 + tris[triIndices[i]].v2) / 3.0f;
            if (centroid[axis] < split) {
                std::swap(triIndices[i], triIndices[mid]);
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
