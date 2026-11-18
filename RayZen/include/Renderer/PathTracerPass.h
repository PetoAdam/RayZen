#ifndef RAYZEN_RENDERER_PATH_TRACER_PASS_H
#define RAYZEN_RENDERER_PATH_TRACER_PASS_H

#include <array>
#include <filesystem>

#include <GL/glew.h>

#include "Renderer/GLResources.h"
#include "Renderer/RenderConfig.h"
#include "Renderer/DebugState.h"
#include "Camera.h"

namespace rayzen {

// Compute-shader-based path tracer. Writes linear HDR radiance to a single
// RGBA32F image (the accumulation image). Does not perform tone mapping or
// overlays; that is handled by DisplayResolvePass.
class PathTracerPass {
public:
    PathTracerPass();
    ~PathTracerPass() = default;

    bool initialize(const std::string& computeShaderPath, ShadingMode mode = ShadingMode::PathTracing);
    void resize(int width, int height);

    // Issues one compute dispatch.
    //   accumulatedFrames: how many frames of history are already in the image
    //                      (zero when resetAccumulation is true)
    //   frameIndex:        seed source for stochastic sampling
    bool dispatch(const Camera& camera,
                  const RenderConfig& config,
                  const DebugState& debug,
                  int totalTriangles,
                  int numLights,
                  bool resetAccumulation,
                  int accumulatedFrames,
                  int frameIndex);

    GLuint accumulationTexture() const noexcept { return accumulation_.id(); }
    int    width() const noexcept { return accumulation_.width(); }
    int    height() const noexcept { return accumulation_.height(); }
    bool   ready() const noexcept { return programs_[activeMode_].valid(); }
    GLuint programId() const noexcept { return programs_[activeMode_].id(); }

private:
    bool selectMode(ShadingMode mode);
    void cacheUniformLocations();

    std::array<GLProgram, 3> programs_;
    size_t activeMode_ = 0;
    std::filesystem::path pathShader_;
    GLTexture accumulation_;

    struct Locs {
        GLint resolution = -1;
        GLint numTriangles = -1;
        GLint numLights = -1;
        GLint bounceBudget = -1;
        GLint shadingMode = -1;
        GLint subsurfaceEnabled = -1;
        GLint causticsEnabled = -1;
        GLint accumulatedFrames = -1;
        GLint resetAccumulation = -1;
        GLint frameIndex = -1;
        GLint ambientLightColor = -1;
        GLint cameraView = -1;
        GLint cameraProj = -1;
        GLint cameraPos = -1;
        GLint cameraInvView = -1;
        GLint cameraInvProj = -1;
    } locs_;

    // Cached workgroup size queried from the program at link time.
    GLint workgroupX_ = 8;
    GLint workgroupY_ = 8;
};

} // namespace rayzen

#endif
