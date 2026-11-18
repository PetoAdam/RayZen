#ifndef RAYZEN_RENDERER_RASTER_PASS_H
#define RAYZEN_RENDERER_RASTER_PASS_H

#include <GL/glew.h>
#include <glm/glm.hpp>
#include <unordered_map>

#include "Renderer/GLResources.h"
#include "Renderer/RenderConfig.h"
#include "Scene.h"
#include "Mesh.h"

namespace rayzen {

// Editor-mode raster pass. Used while the path tracer compiles asynchronously
// or when the user toggles editor mode (F1).
class RasterPass {
public:
    RasterPass();
    ~RasterPass();

    bool initialize(const std::string& vertPath, const std::string& fragPath);

    // Builds VAO/VBOs for every unique mesh in the scene.
    void buildSceneGeometry(const Scene& scene);

    void render(const Scene& scene, const RenderConfig& config);

private:
    struct MeshGPU {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLsizei vertexCount = 0;
    };

    void cacheUniformLocations();
    void releaseMeshes();

    GLProgram program_;
    std::unordered_map<const Mesh*, MeshGPU> cache_;

    struct Locs {
        GLint view = -1;
        GLint proj = -1;
        GLint cameraPos = -1;
        GLint numLights = -1;
        GLint ambientColor = -1;
        GLint model = -1;
        GLint normalMatrix = -1;
    } locs_;
};

} // namespace rayzen

#endif
