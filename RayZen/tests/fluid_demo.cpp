#include <cmath>
#include <iostream>
#include <vector>

#include <GL/glew.h>
#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>

#include "Renderer/FluidDemo.h"

int main() {
    if (!glfwInit()) return 77;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    GLFWwindow* window = glfwCreateWindow(256, 192, "fluid test", nullptr, nullptr);
    if (!window) { glfwTerminate(); return 77; }
    glfwMakeContextCurrent(window);
    glewExperimental = GL_TRUE;
    if (glewInit() != GLEW_OK) { glfwDestroyWindow(window); glfwTerminate(); return 77; }
    glGetError();

    bool success = true;
    for (auto dimension : {rayzen::FluidDimension::Two, rayzen::FluidDimension::Three}) {
        rayzen::FluidDemo demo;
        if (!demo.initialize(std::filesystem::path(RAYZEN_SOURCE_DIR) / "shaders", dimension)) {
            success = false;
            break;
        }
        std::vector<rayzen::FluidParticle> initial, moved;
        if (!demo.readParticles(initial) || !demo.render(256, 192)) { success = false; break; }
        std::vector<unsigned char> before(256 * 192 * 4), after(before.size());
        glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, before.data());
        if (dimension == rayzen::FluidDimension::Three) {
            std::vector<rayzen::FluidParticle> outside = initial;
            for (auto& particle : outside) particle.position = glm::vec4(100, 100, 100, 1);
            GLint buffer = 0;
            glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 0, &buffer);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
            glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, outside.size() * sizeof(outside[0]), outside.data());
            if (!demo.render(256, 192)) { success = false; break; }
            glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, after.data());
            size_t fluidPixels = 0;
            for (size_t i = 0; i < before.size(); i += 4)
                if (std::abs(int(before[i]) - int(after[i])) +
                    std::abs(int(before[i + 1]) - int(after[i + 1])) +
                    std::abs(int(before[i + 2]) - int(after[i + 2])) > 10) ++fluidPixels;
            if (fluidPixels < 100 || !demo.reset()) { success = false; break; }
        }
        for (int step = 0; step < 240; ++step) {
            if (!demo.step(1.0f / 120.0f)) { success = false; break; }
        }
        if (!success || !demo.readParticles(moved) || !demo.render(256, 192)) { success = false; break; }
        glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, after.data());
        if (initial.size() != moved.size() || initial.empty()) { success = false; break; }
        bool movement = false;
        const glm::mat4 inverseObstacle = glm::rotate(glm::mat4(1.0f), -239.0f / 120.0f * 0.65f,
                                                       glm::vec3(0, 1, 0));
        for (size_t i = 0; i < moved.size(); ++i) {
            const auto& p = moved[i].position;
            const auto& v = moved[i].velocity;
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z) ||
                !std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) ||
                p.x < -2.0f || p.x > 2.0f || p.y < -1.65f || p.y > 1.8f ||
                p.z < -1.2f || p.z > 1.2f ||
                (dimension == rayzen::FluidDimension::Two && (p.z != 0.0f || v.z != 0.0f))) {
                success = false;
                break;
            }
            if (dimension == rayzen::FluidDimension::Three) {
                const glm::vec3 local = glm::abs(glm::vec3(inverseObstacle *
                    glm::vec4(glm::vec3(p) - glm::vec3(0, -1.02f, 0), 0.0f)));
                if (local.x < 0.169f && local.y < 0.619f && local.z < 0.479f) {
                    success = false;
                    break;
                }
            }
            movement = movement || glm::length(glm::vec3(p - initial[i].position)) > 0.01f;
        }
        size_t changed = 0;
        for (size_t i = 0; i < before.size(); i += 4)
            if (std::abs(int(before[i]) - int(after[i])) +
                std::abs(int(before[i + 1]) - int(after[i + 1])) +
                std::abs(int(before[i + 2]) - int(after[i + 2])) > 10) ++changed;
        if (!movement || changed < 50 || glGetError() != GL_NO_ERROR) success = false;
        std::vector<rayzen::FluidParticle> unforced, forced;
        if (!demo.reset() || !demo.step(1.0f / 120.0f) || !demo.readParticles(unforced) ||
            !demo.reset()) { success = false; break; }
        demo.setInteraction(glm::vec3(-0.8f, -0.5f, 0.0f), 18.0f);
        if (!demo.step(1.0f / 120.0f) || !demo.readParticles(forced)) { success = false; break; }
        bool interactionWorked = false;
        for (size_t i = 0; i < forced.size(); ++i)
            interactionWorked = interactionWorked ||
                glm::length(glm::vec3(forced[i].velocity - unforced[i].velocity)) > 0.01f;
        if (!interactionWorked) success = false;
        if (!success) break;
    }
    glfwDestroyWindow(window);
    glfwTerminate();
    if (!success) std::cerr << "Fluid simulation or rendering regression failed\n";
    return success ? 0 : 1;
}
