#ifndef RAYZEN_RENDERER_LEGACY_FRAGMENT_PASS_H
#define RAYZEN_RENDERER_LEGACY_FRAGMENT_PASS_H

#include <GL/glew.h>

#include "Renderer/GLResources.h"
#include "Renderer/RenderConfig.h"
#include "Renderer/DebugState.h"
#include "Scene.h"

namespace rayzen {

// Wraps the original fragment-shader path tracer (shaders/fragment_shader.glsl).
// Kept as a runtime fallback per the compute migration plan: useful for image
// parity comparisons and as a safety net if the compute backend is unavailable.
class LegacyFragmentPass {
public:
    LegacyFragmentPass();
    ~LegacyFragmentPass();

    bool initialize(const std::string& vertPath, const std::string& fragPath);

    // Adopts a program compiled on a worker thread (e.g. async pipeline warm-up).
    void adoptProgram(GLuint program);

    void resize(int width, int height);

    // Renders one frame to the default framebuffer.
    void render(const Scene& scene,
                const RenderConfig& config,
                const DebugState& debug,
                int totalTriangles,
                bool resetAccumulation,
                int accumulatedFrames,
                int frameIndex);

    bool ready() const noexcept { return program_.valid(); }

private:
    void cacheUniformLocations();

    GLProgram program_;
    GLTexture accumulation_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;

    struct Locs {
        GLint resolution = -1;
        GLint numTriangles = -1;
        GLint numLights = -1;
        GLint bounceBudget = -1;
        GLint shadingMode = -1;
        GLint subsurfaceEnabled = -1;
        GLint accumulatedFrames = -1;
        GLint resetAccumulation = -1;
        GLint frameIndex = -1;
        GLint useImageAccumulation = -1;
        GLint debugShowLights = -1;
        GLint debugShowBVH = -1;
        GLint debugBVHMode = -1;
        GLint debugSelectedBLAS = -1;
        GLint debugSelectedTri = -1;
        GLint exposure = -1;
        GLint ambientLightColor = -1;
        GLint cameraView = -1;
        GLint cameraProj = -1;
        GLint cameraPos = -1;
        GLint cameraInvView = -1;
        GLint cameraInvProj = -1;
    } locs_;
};

} // namespace rayzen

#endif
