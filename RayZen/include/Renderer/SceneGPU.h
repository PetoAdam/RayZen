#ifndef RAYZEN_RENDERER_SCENE_GPU_H
#define RAYZEN_RENDERER_SCENE_GPU_H

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <filesystem>
#include <vector>

#include "Scene.h"
#include "BVH.h"

namespace rayzen {

enum class SceneUpdate { Unchanged, Transforms, Scene, Error };

// Owns every SSBO that holds scene data: triangles, materials, lights, TLAS, BLAS,
// BVH instances. Handles BVH cache load/save and dynamic TLAS rebuilds.
//
// SSBO bindings (must match path_trace.comp / display_resolve.frag / fragment_shader.glsl):
//   0 = triangles
//   1 = materials
//   2 = lights
//   5 = TLAS nodes
//   6 = TLAS leaf-instance indices
//   7 = BLAS nodes
//   8 = BLAS triangle indices
//   9 = BVH instances
class SceneGPU {
public:
    SceneGPU();
    ~SceneGPU();

    SceneGPU(const SceneGPU&) = delete;
    SceneGPU& operator=(const SceneGPU&) = delete;

    // Initial upload. Builds or loads cached BVH for every scene mesh.
    // Returns false on a fatal upload failure.
    bool initialize(const Scene& scene, bool forceRebuildBVH, bool cacheOnly = false,
                    bool requireCacheWrite = false, bool persistCache = true);

    // Bind every SSBO to its expected binding point. Call once after initialize().
    void bindAll() const;

    // Refreshes the TLAS (and BVH instance buffer) when game-object transforms change.
    // Cheap when transforms are unchanged; reuses cached BLAS data.
    SceneUpdate updateDynamic(const Scene& scene);

    // Re-uploads the lights buffer (used when render config changes light power).
    void uploadLights(const Scene& scene);

    int totalTriangles() const noexcept { return totalTriangles_; }

private:
    void swap(SceneGPU& other) noexcept;
    bool uploadTextures(const Scene& scene);
    // SSBO IDs.
    GLuint triangleSSBO_ = 0;
    GLuint materialSSBO_ = 0;
    GLuint lightSSBO_ = 0;
    GLuint tlasNodeSSBO_ = 0;
    GLuint tlasTriIdxSSBO_ = 0;
    GLuint blasNodeSSBO_ = 0;
    GLuint blasTriIdxSSBO_ = 0;
    GLuint bvhInstanceSSBO_ = 0;
    GLuint textureArray_ = 0;

    int totalTriangles_ = 0;

    // BLAS data kept on the CPU for dynamic TLAS rebuilds.
    std::vector<BVH> meshBLAS_;
    std::vector<BVHNode> meshRootNodes_;
    std::vector<glm::mat4> previousTransforms_;
    std::vector<std::shared_ptr<Mesh>> previousMeshes_;
    std::vector<uint64_t> meshFingerprints_;
    uint64_t materialsFingerprint_ = 0;
    uint64_t lightsFingerprint_ = 0;
    uint64_t texturesFingerprint_ = 0;
    std::filesystem::path cacheDir_;
};

} // namespace rayzen

#endif
