#include "Renderer/LegacyFragmentPass.h"

#include <glm/gtc/type_ptr.hpp>

#include "Logger.h"

namespace rayzen {

LegacyFragmentPass::LegacyFragmentPass() = default;

LegacyFragmentPass::~LegacyFragmentPass() {
    if (vbo_ != 0) glDeleteBuffers(1, &vbo_);
    if (vao_ != 0) glDeleteVertexArrays(1, &vao_);
}

bool LegacyFragmentPass::initialize(const std::string& vertPath, const std::string& fragPath) {
    if (!program_.loadGraphics(vertPath, fragPath)) {
        Logger::error("LegacyFragmentPass: failed to load fragment-tracer shader");
        return false;
    }
    cacheUniformLocations();

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

void LegacyFragmentPass::adoptProgram(GLuint program) {
    program_.adopt(program);
    cacheUniformLocations();
    if (vao_ == 0) {
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
    }
}

void LegacyFragmentPass::cacheUniformLocations() {
    locs_ = {};
    if (!program_.valid()) return;
    locs_.resolution = program_.uniformLocation("resolution");
    locs_.numTriangles = program_.uniformLocation("numTriangles");
    locs_.numLights = program_.uniformLocation("numLights");
    locs_.bounceBudget = program_.uniformLocation("uniformBounceBudget");
    locs_.shadingMode = program_.uniformLocation("uniformShadingMode");
    locs_.subsurfaceEnabled = program_.uniformLocation("uniformSubsurfaceEnabled");
    locs_.accumulatedFrames = program_.uniformLocation("uniformAccumulatedFrames");
    locs_.resetAccumulation = program_.uniformLocation("uniformResetAccumulation");
    locs_.frameIndex = program_.uniformLocation("uniformFrameIndex");
    locs_.useImageAccumulation = program_.uniformLocation("uniformUseImageAccumulation");
    locs_.debugShowLights = program_.uniformLocation("debugShowLights");
    locs_.debugShowBVH = program_.uniformLocation("debugShowBVH");
    locs_.debugBVHMode = program_.uniformLocation("debugBVHMode");
    locs_.debugSelectedBLAS = program_.uniformLocation("debugSelectedBLAS");
    locs_.debugSelectedTri = program_.uniformLocation("debugSelectedTri");
    locs_.exposure = program_.uniformLocation("uniformDisplayExposure");
    locs_.ambientLightColor = program_.uniformLocation("uniformAmbientLightColor");
    locs_.cameraView = program_.uniformLocation("camera.viewMatrix");
    locs_.cameraProj = program_.uniformLocation("camera.projectionMatrix");
    locs_.cameraPos = program_.uniformLocation("camera.position");
    locs_.cameraInvView = program_.uniformLocation("camera.invViewMatrix");
    locs_.cameraInvProj = program_.uniformLocation("camera.invProjectionMatrix");
}

void LegacyFragmentPass::resize(int width, int height) {
    accumulation_.ensureRGBA32F(width, height);
}

void LegacyFragmentPass::render(const Scene& scene,
                                const RenderConfig& config,
                                const DebugState& debug,
                                int totalTriangles,
                                bool resetAccumulation,
                                int accumulatedFrames,
                                int frameIndex) {
    if (!program_.valid() || accumulation_.id() == 0) return;

    program_.use();
    glBindImageTexture(0, accumulation_.id(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

    const int W = accumulation_.width();
    const int H = accumulation_.height();

    if (locs_.resolution >= 0) glUniform2f(locs_.resolution, float(W), float(H));
    if (locs_.numTriangles >= 0) glUniform1i(locs_.numTriangles, totalTriangles);
    if (locs_.numLights >= 0) glUniform1i(locs_.numLights, static_cast<int>(scene.lights.size()));
    if (locs_.bounceBudget >= 0) glUniform1i(locs_.bounceBudget, config.pathTracerBounceBudget);
    if (locs_.shadingMode >= 0) glUniform1i(locs_.shadingMode, static_cast<int>(config.shadingMode));
    if (locs_.subsurfaceEnabled >= 0) glUniform1i(locs_.subsurfaceEnabled, config.subsurfaceEnabled);
    if (locs_.accumulatedFrames >= 0)
        glUniform1i(locs_.accumulatedFrames, config.accumulationEnabled ? accumulatedFrames : 0);
    if (locs_.resetAccumulation >= 0) glUniform1i(locs_.resetAccumulation, resetAccumulation ? 1 : 0);
    if (locs_.frameIndex >= 0) glUniform1i(locs_.frameIndex, frameIndex);
    if (locs_.useImageAccumulation >= 0) {
        // WSL D3D12 can render black with image load/store in the fragment path.
        // Use direct radiance output in the legacy fallback for stability.
        glUniform1i(locs_.useImageAccumulation, 0);
    }
    if (locs_.debugShowLights >= 0) glUniform1i(locs_.debugShowLights, debug.showLights ? 1 : 0);
    if (locs_.debugShowBVH >= 0) glUniform1i(locs_.debugShowBVH, debug.showBVH ? 1 : 0);
    if (locs_.debugBVHMode >= 0) glUniform1i(locs_.debugBVHMode, debug.bvhMode);
    if (locs_.debugSelectedBLAS >= 0) glUniform1i(locs_.debugSelectedBLAS, debug.selectedBLAS);
    if (locs_.debugSelectedTri >= 0) glUniform1i(locs_.debugSelectedTri, debug.selectedTri);
    if (locs_.exposure >= 0) glUniform1f(locs_.exposure, config.displayExposure);
    if (locs_.ambientLightColor >= 0) {
        const float a = config.pathTracerAmbient;
        glUniform3f(locs_.ambientLightColor, a, a, a);
    }
    if (locs_.cameraView >= 0)
        glUniformMatrix4fv(locs_.cameraView, 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    if (locs_.cameraProj >= 0)
        glUniformMatrix4fv(locs_.cameraProj, 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    if (locs_.cameraPos >= 0)
        glUniform3fv(locs_.cameraPos, 1, glm::value_ptr(scene.camera.position));
    if (locs_.cameraInvView >= 0) {
        glm::mat4 inv = glm::inverse(scene.camera.viewMatrix);
        glUniformMatrix4fv(locs_.cameraInvView, 1, GL_FALSE, glm::value_ptr(inv));
    }
    if (locs_.cameraInvProj >= 0) {
        glm::mat4 inv = glm::inverse(scene.camera.projectionMatrix);
        glUniformMatrix4fv(locs_.cameraInvProj, 1, GL_FALSE, glm::value_ptr(inv));
    }

    glDisable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glBindVertexArray(vao_);
    // TRIANGLE_STRIP is more robust across GL-on-D3D12 translators than TRIANGLE_FAN.
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    ::glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glBindVertexArray(0);
}

} // namespace rayzen
