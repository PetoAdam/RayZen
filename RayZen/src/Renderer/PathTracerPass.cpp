#include "Renderer/PathTracerPass.h"

#include <glm/gtc/type_ptr.hpp>

#include "Logger.h"

namespace rayzen {

PathTracerPass::PathTracerPass() = default;

bool PathTracerPass::initialize(const std::string& computeShaderPath, ShadingMode mode) {
    pathShader_ = computeShaderPath;
    return selectMode(mode);
}

bool PathTracerPass::selectMode(ShadingMode mode) {
    const auto index = static_cast<size_t>(mode);
    if (index >= programs_.size()) {
        Logger::error("PathTracerPass: invalid shading mode");
        return false;
    }
    if (index == activeMode_ && programs_[index].valid()) return true;
    const char* names[] = {"path_trace.comp", "realtime_ray.comp", "lambertian.comp"};
    const std::string path = (index == 0 ? pathShader_ : pathShader_.parent_path() / names[index]).string();
    if (!programs_[index].valid()) {
        Logger::info("Preparing compute shading mode: " + std::string(names[index]));
        if (!programs_[index].loadCompute(path)) {
            Logger::error("PathTracerPass: failed to load compute shader: " + path);
            return false;
        }
    }
    activeMode_ = index;
    cacheUniformLocations();

    GLint groups[3] = {8, 8, 1};
    glGetProgramiv(programs_[activeMode_].id(), GL_COMPUTE_WORK_GROUP_SIZE, groups);
    if (groups[0] > 0) workgroupX_ = groups[0];
    if (groups[1] > 0) workgroupY_ = groups[1];
    return true;
}

void PathTracerPass::cacheUniformLocations() {
    const GLProgram& program_ = programs_[activeMode_];
    locs_ = {};
    locs_.resolution = program_.uniformLocation("resolution");
    locs_.numTriangles = program_.uniformLocation("numTriangles");
    locs_.numLights = program_.uniformLocation("numLights");
    locs_.bounceBudget = program_.uniformLocation("uniformBounceBudget");
    locs_.shadingMode = program_.uniformLocation("uniformShadingMode");
    locs_.subsurfaceEnabled = program_.uniformLocation("uniformSubsurfaceEnabled");
    locs_.causticsEnabled = program_.uniformLocation("uniformCausticsEnabled");
    locs_.accumulatedFrames = program_.uniformLocation("uniformAccumulatedFrames");
    locs_.resetAccumulation = program_.uniformLocation("uniformResetAccumulation");
    locs_.frameIndex = program_.uniformLocation("uniformFrameIndex");
    locs_.ambientLightColor = program_.uniformLocation("uniformAmbientLightColor");
    locs_.cameraView = program_.uniformLocation("camera.viewMatrix");
    locs_.cameraProj = program_.uniformLocation("camera.projectionMatrix");
    locs_.cameraPos = program_.uniformLocation("camera.position");
    locs_.cameraInvView = program_.uniformLocation("camera.invViewMatrix");
    locs_.cameraInvProj = program_.uniformLocation("camera.invProjectionMatrix");
}

void PathTracerPass::resize(int width, int height) {
    if (accumulation_.ensureRGBA32F(width, height)) {
        // Reallocation is treated as an accumulation reset by the caller via Renderer.
    }
}

bool PathTracerPass::dispatch(const Camera& camera,
                              const RenderConfig& config,
                              const DebugState& /*debug*/,
                              int totalTriangles,
                              int numLights,
                              bool resetAccumulation,
                              int accumulatedFrames,
                              int frameIndex) {
    if (accumulation_.id() == 0 || !selectMode(config.shadingMode)) return false;

    const GLProgram& program_ = programs_[activeMode_];
    program_.use();
    glBindImageTexture(0, accumulation_.id(), 0, GL_FALSE, 0, GL_READ_WRITE, GL_RGBA32F);

    const int W = accumulation_.width();
    const int H = accumulation_.height();

    if (locs_.resolution >= 0) glUniform2f(locs_.resolution, float(W), float(H));
    if (locs_.numTriangles >= 0) glUniform1i(locs_.numTriangles, totalTriangles);
    if (locs_.numLights >= 0) glUniform1i(locs_.numLights, numLights);
    if (locs_.bounceBudget >= 0) glUniform1i(locs_.bounceBudget, config.pathTracerBounceBudget);
    if (locs_.shadingMode >= 0) glUniform1i(locs_.shadingMode, static_cast<int>(config.shadingMode));
    if (locs_.subsurfaceEnabled >= 0) glUniform1i(locs_.subsurfaceEnabled, config.subsurfaceEnabled);
    if (locs_.causticsEnabled >= 0) glUniform1i(locs_.causticsEnabled, config.causticsEnabled);
    if (locs_.accumulatedFrames >= 0)
        glUniform1i(locs_.accumulatedFrames, config.accumulationEnabled ? accumulatedFrames : 0);
    if (locs_.resetAccumulation >= 0) glUniform1i(locs_.resetAccumulation, resetAccumulation ? 1 : 0);
    if (locs_.frameIndex >= 0) glUniform1i(locs_.frameIndex, frameIndex);
    if (locs_.ambientLightColor >= 0) {
        const float a = config.pathTracerAmbient;
        glUniform3f(locs_.ambientLightColor, a, a, a);
    }
    if (locs_.cameraView >= 0)
        glUniformMatrix4fv(locs_.cameraView, 1, GL_FALSE, glm::value_ptr(camera.viewMatrix));
    if (locs_.cameraProj >= 0)
        glUniformMatrix4fv(locs_.cameraProj, 1, GL_FALSE, glm::value_ptr(camera.projectionMatrix));
    if (locs_.cameraPos >= 0)
        glUniform3fv(locs_.cameraPos, 1, glm::value_ptr(camera.position));
    if (locs_.cameraInvView >= 0) {
        glm::mat4 inv = glm::inverse(camera.viewMatrix);
        glUniformMatrix4fv(locs_.cameraInvView, 1, GL_FALSE, glm::value_ptr(inv));
    }
    if (locs_.cameraInvProj >= 0) {
        glm::mat4 inv = glm::inverse(camera.projectionMatrix);
        glUniformMatrix4fv(locs_.cameraInvProj, 1, GL_FALSE, glm::value_ptr(inv));
    }

    const GLuint groupsX = (static_cast<GLuint>(W) + workgroupX_ - 1) / workgroupX_;
    const GLuint groupsY = (static_cast<GLuint>(H) + workgroupY_ - 1) / workgroupY_;
    glDispatchCompute(groupsX, groupsY, 1);
    // Ensure the resolve pass sees the writes via texture sampling.
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);
    return true;
}

} // namespace rayzen
