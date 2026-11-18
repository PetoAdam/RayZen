#include "Renderer/CausticPass.h"

#include <algorithm>
#include <limits>
#include <glm/gtc/type_ptr.hpp>

#include "Logger.h"

namespace rayzen {

CausticPass::~CausticPass() {
    if (pixels_) glDeleteBuffers(1, &pixels_);
    if (texture_) glDeleteTextures(1, &texture_);
}

bool CausticPass::initialize(const std::string& path) {
    GLint bindings = 0;
    glGetIntegerv(GL_MAX_IMAGE_UNITS, &bindings);
    if (bindings <= 2) {
        Logger::error("Caustics require at least three image units");
        return false;
    }
    if (!program_.loadCompute(path)) return false;
    glGenBuffers(1, &pixels_);
    glGenTextures(1, &texture_);
    return pixels_ != 0 && texture_ != 0;
}

bool CausticPass::render(const Scene& scene, const RenderConfig& config, int width, int height, int frame) {
    if (!program_.valid() || !pixels_ || !texture_ || width <= 0 || height <= 0) return false;
    const GLsizeiptr bytes = static_cast<GLsizeiptr>(width) * height * 4 * sizeof(GLuint);
    if (width != width_ || height != height_) {
        GLint maxTexels = 0;
        glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &maxTexels);
        if (bytes / sizeof(GLuint) > maxTexels) {
            Logger::error("Caustic buffer exceeds the maximum texture buffer size");
            return false;
        }
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, pixels_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, bytes, nullptr, GL_DYNAMIC_DRAW);
        glBindTexture(GL_TEXTURE_BUFFER, texture_);
        glTexBuffer(GL_TEXTURE_BUFFER, GL_R32UI, pixels_);
        if (glGetError() != GL_NO_ERROR) {
            Logger::error("Failed to allocate caustic buffer");
            return false;
        }
        width_ = width;
        height_ = height;
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, pixels_);
    const GLuint zero = 0;
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
    glBindImageTexture(2, texture_, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT | GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    program_.use();
    const auto setInt = [&](const char* name, int value) {
        const GLint location = program_.uniformLocation(name);
        if (location >= 0) glUniform1i(location, value);
    };
    const GLint resolution = program_.uniformLocation("resolution");
    if (resolution >= 0) glUniform2f(resolution, float(width), float(height));
    const GLint view = program_.uniformLocation("camera.viewMatrix");
    const GLint projection = program_.uniformLocation("camera.projectionMatrix");
    const GLint position = program_.uniformLocation("camera.position");
    if (view >= 0) glUniformMatrix4fv(view, 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    if (projection >= 0) glUniformMatrix4fv(projection, 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    if (position >= 0) glUniform3fv(position, 1, glm::value_ptr(scene.camera.position));
    setInt("uniformShadingMode", static_cast<int>(config.shadingMode));
    setInt("uniformFrameIndex", frame);
    const int photons = config.causticPhotonCount;
    setInt("causticPhotonCount", photons);
    for (size_t i = 0; i < scene.gameObjects.size(); ++i) {
        const auto& object = scene.gameObjects[i];
        if (!object.mesh) continue;
        bool transparent = false;
        glm::vec3 lo(std::numeric_limits<float>::max()), hi(-std::numeric_limits<float>::max());
        for (const Triangle& triangle : object.mesh->triangles) {
            if (triangle.materialIndex >= 0 && static_cast<size_t>(triangle.materialIndex) < scene.materials.size() &&
                scene.materials[triangle.materialIndex].transparency > 0.0f) transparent = true;
            for (const glm::vec3* vertex : {&triangle.v0, &triangle.v1, &triangle.v2}) {
                const glm::vec3 p(object.transform * glm::vec4(*vertex, 1.0f));
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        }
        if (!transparent) continue;
        const glm::vec3 center = (lo + hi) * 0.5f;
        const float radius = glm::length(hi - lo) * 0.5f + 0.004f;
        const GLint centerLoc = program_.uniformLocation("causticTargetCenter");
        const GLint radiusLoc = program_.uniformLocation("causticTargetRadius");
        if (centerLoc >= 0) glUniform3fv(centerLoc, 1, glm::value_ptr(center));
        if (radiusLoc >= 0) glUniform1f(radiusLoc, radius);
        setInt("causticTargetInstance", static_cast<int>(i));
        for (size_t light = 0; light < scene.lights.size(); ++light) {
            setInt("causticLightIndex", static_cast<int>(light));
            glDispatchCompute(static_cast<GLuint>((photons + 63) / 64), 1, 1);
        }
    }
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    if (glGetError() != GL_NO_ERROR) {
        Logger::error("Caustics dispatch failed");
        return false;
    }
    return true;
}

}
