#include "Renderer/RasterPass.h"

#include <cstddef>
#include <cmath>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Logger.h"

namespace rayzen {

namespace {

struct RasterVertex {
    glm::vec3 position;
    glm::vec3 normal;
    int materialIndex;
    glm::vec2 uv;
};

} // namespace

RasterPass::RasterPass() = default;

RasterPass::~RasterPass() {
    releaseMeshes();
}

bool RasterPass::initialize(const std::string& vertPath, const std::string& fragPath) {
    if (!program_.loadGraphics(vertPath, fragPath)) {
        Logger::error("RasterPass: failed to load editor shader");
        return false;
    }
    cacheUniformLocations();
    return true;
}

void RasterPass::cacheUniformLocations() {
    locs_ = {};
    locs_.view = program_.uniformLocation("uView");
    locs_.proj = program_.uniformLocation("uProj");
    locs_.cameraPos = program_.uniformLocation("uCameraPos");
    locs_.numLights = program_.uniformLocation("numLights");
    locs_.ambientColor = program_.uniformLocation("uAmbientColor");
    locs_.model = program_.uniformLocation("uModel");
    locs_.normalMatrix = program_.uniformLocation("uNormalMatrix");
}

void RasterPass::buildSceneGeometry(const Scene& scene) {
    releaseMeshes();
    for (const auto& obj : scene.gameObjects) {
        const Mesh* meshPtr = obj.mesh.get();
        if (!meshPtr) continue;
        if (cache_.find(meshPtr) != cache_.end()) continue;
        if (meshPtr->triangles.empty()) continue;

        std::vector<RasterVertex> vertices;
        vertices.reserve(meshPtr->triangles.size() * 3);
        for (const auto& tri : meshPtr->triangles) {
            glm::vec3 p0 = tri.v0;
            glm::vec3 p1 = tri.v1;
            glm::vec3 p2 = tri.v2;
            glm::vec3 normal = glm::normalize(glm::cross(p1 - p0, p2 - p0));
            if (!std::isfinite(normal.x) || !std::isfinite(normal.y) || !std::isfinite(normal.z) ||
                glm::length(normal) < 1e-5f) {
                normal = glm::vec3(0.0f, 1.0f, 0.0f);
            }
            const glm::vec2 missingUV(-1e30f);
            vertices.push_back({p0, normal, tri.materialIndex, tri.hasUV ? tri.uv0 : missingUV});
            vertices.push_back({p1, normal, tri.materialIndex, tri.hasUV ? tri.uv1 : missingUV});
            vertices.push_back({p2, normal, tri.materialIndex, tri.hasUV ? tri.uv2 : missingUV});
        }
        if (vertices.empty()) continue;

        MeshGPU gpu;
        glGenVertexArrays(1, &gpu.vao);
        glGenBuffers(1, &gpu.vbo);
        glBindVertexArray(gpu.vao);
        glBindBuffer(GL_ARRAY_BUFFER, gpu.vbo);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(RasterVertex), vertices.data(), GL_STATIC_DRAW);

        const GLsizei stride = sizeof(RasterVertex);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(RasterVertex, position)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(RasterVertex, normal)));
        glEnableVertexAttribArray(2);
        glVertexAttribIPointer(2, 1, GL_INT, stride,
                               reinterpret_cast<void*>(offsetof(RasterVertex, materialIndex)));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(RasterVertex, uv)));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

        gpu.vertexCount = static_cast<GLsizei>(vertices.size());
        cache_[meshPtr] = gpu;
    }
}

void RasterPass::render(const Scene& scene, const RenderConfig& config) {
    glEnable(GL_DEPTH_TEST);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    program_.use();
    if (locs_.view >= 0)
        glUniformMatrix4fv(locs_.view, 1, GL_FALSE, glm::value_ptr(scene.camera.viewMatrix));
    if (locs_.proj >= 0)
        glUniformMatrix4fv(locs_.proj, 1, GL_FALSE, glm::value_ptr(scene.camera.projectionMatrix));
    if (locs_.cameraPos >= 0)
        glUniform3fv(locs_.cameraPos, 1, glm::value_ptr(scene.camera.position));
    if (locs_.numLights >= 0)
        glUniform1i(locs_.numLights, static_cast<int>(scene.lights.size()));
    if (locs_.ambientColor >= 0) {
        const float a = config.rasterAmbient;
        glUniform3f(locs_.ambientColor, a, a, a);
    }

    for (const auto& obj : scene.gameObjects) {
        const Mesh* meshPtr = obj.mesh.get();
        auto it = cache_.find(meshPtr);
        if (it == cache_.end()) continue;
        const MeshGPU& gpu = it->second;
        if (gpu.vertexCount == 0) continue;

        glm::mat4 model = obj.transform;
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(model)));
        if (locs_.model >= 0)
            glUniformMatrix4fv(locs_.model, 1, GL_FALSE, glm::value_ptr(model));
        if (locs_.normalMatrix >= 0)
            glUniformMatrix3fv(locs_.normalMatrix, 1, GL_FALSE, glm::value_ptr(normalMatrix));

        glBindVertexArray(gpu.vao);
        glDrawArrays(GL_TRIANGLES, 0, gpu.vertexCount);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void RasterPass::releaseMeshes() {
    for (auto& kv : cache_) {
        if (kv.second.vbo != 0) glDeleteBuffers(1, &kv.second.vbo);
        if (kv.second.vao != 0) glDeleteVertexArrays(1, &kv.second.vao);
    }
    cache_.clear();
}

} // namespace rayzen
