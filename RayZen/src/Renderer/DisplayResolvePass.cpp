#include "Renderer/DisplayResolvePass.h"

#include <glm/gtc/type_ptr.hpp>

#include "Logger.h"

namespace rayzen {

DisplayResolvePass::DisplayResolvePass() = default;

DisplayResolvePass::~DisplayResolvePass() {
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
}

bool DisplayResolvePass::initialize(const std::string& vertPath, const std::string& fragPath) {
    if (!program_.loadGraphics(vertPath, fragPath)) {
        Logger::error("DisplayResolvePass: failed to load resolve shaders");
        return false;
    }
    cacheUniformLocations();

    // Fullscreen quad geometry.
    const float quadVertices[] = {
        -1.0f,  1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
    };
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), nullptr);
    glBindVertexArray(0);
    return true;
}

void DisplayResolvePass::cacheUniformLocations() {
    locs_ = {};
    locs_.resolution = program_.uniformLocation("resolution");
    locs_.exposure = program_.uniformLocation("uniformDisplayExposure");
    locs_.accumulationSampler = program_.uniformLocation("accumulationTex");
    locs_.numLights = program_.uniformLocation("numLights");
    locs_.debugShowLights = program_.uniformLocation("debugShowLights");
    locs_.debugShowBVH = program_.uniformLocation("debugShowBVH");
    locs_.debugBVHMode = program_.uniformLocation("debugBVHMode");
    locs_.debugSelectedBLAS = program_.uniformLocation("debugSelectedBLAS");
    locs_.debugSelectedTri = program_.uniformLocation("debugSelectedTri");
    locs_.cameraView = program_.uniformLocation("camera.viewMatrix");
    locs_.cameraProj = program_.uniformLocation("camera.projectionMatrix");
}

void DisplayResolvePass::render(GLuint accumulationTex,
                                int viewportWidth,
                                int viewportHeight,
                                const Scene& scene,
                                const RenderConfig& config,
                                const DebugState& debug) {
    program_.use();

    // Bind accumulation texture to texture unit 0 for sampling.
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, accumulationTex);
    if (locs_.accumulationSampler >= 0) glUniform1i(locs_.accumulationSampler, 0);

    if (locs_.resolution >= 0)
        glUniform2f(locs_.resolution, float(viewportWidth), float(viewportHeight));
    if (locs_.exposure >= 0) glUniform1f(locs_.exposure, config.displayExposure);
    if (locs_.numLights >= 0) glUniform1i(locs_.numLights, static_cast<int>(scene.lights.size()));
    if (locs_.debugShowLights >= 0) glUniform1i(locs_.debugShowLights, debug.showLights ? 1 : 0);
    if (locs_.debugShowBVH >= 0) glUniform1i(locs_.debugShowBVH, debug.showBVH ? 1 : 0);
    if (locs_.debugBVHMode >= 0) glUniform1i(locs_.debugBVHMode, debug.bvhMode);
    if (locs_.debugSelectedBLAS >= 0) glUniform1i(locs_.debugSelectedBLAS, debug.selectedBLAS);
    if (locs_.debugSelectedTri >= 0) glUniform1i(locs_.debugSelectedTri, debug.selectedTri);
    if (locs_.cameraView >= 0)
        glUniformMatrix4fv(locs_.cameraView, 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    if (locs_.cameraProj >= 0)
        glUniformMatrix4fv(locs_.cameraProj, 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));

    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindVertexArray(vao_);
    // TRIANGLE_STRIP is more robust across GL-on-D3D12 translators than TRIANGLE_FAN.
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

} // namespace rayzen
