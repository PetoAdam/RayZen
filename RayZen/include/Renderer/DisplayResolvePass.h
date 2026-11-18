#ifndef RAYZEN_RENDERER_DISPLAY_RESOLVE_PASS_H
#define RAYZEN_RENDERER_DISPLAY_RESOLVE_PASS_H

#include <GL/glew.h>

#include "Renderer/GLResources.h"
#include "Renderer/RenderConfig.h"
#include "Renderer/DebugState.h"
#include "Scene.h"

namespace rayzen {

// Reads the HDR accumulation image, applies exposure + ACES tone map + gamma,
// and draws debug overlays (BVH wireframe and light markers).
// One full-screen draw per frame.
class DisplayResolvePass {
public:
    DisplayResolvePass();
    ~DisplayResolvePass();

    bool initialize(const std::string& vertPath, const std::string& fragPath);

    // Renders to the default framebuffer (the swap chain).
    //   accumulationTex: GL texture written by PathTracerPass.
    void render(GLuint accumulationTex,
                int viewportWidth,
                int viewportHeight,
                const Scene& scene,
                const RenderConfig& config,
                const DebugState& debug);

private:
    void cacheUniformLocations();

    GLProgram program_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;

    struct Locs {
        GLint resolution = -1;
        GLint exposure = -1;
        GLint accumulationSampler = -1;
        GLint numLights = -1;
        GLint debugShowLights = -1;
        GLint debugShowBVH = -1;
        GLint debugBVHMode = -1;
        GLint debugSelectedBLAS = -1;
        GLint debugSelectedTri = -1;
        GLint cameraView = -1;
        GLint cameraProj = -1;
    } locs_;
};

} // namespace rayzen

#endif
