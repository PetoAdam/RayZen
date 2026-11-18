#include "Renderer/FluidDemo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "Logger.h"

namespace rayzen {
namespace {
constexpr float smoothingRadius = 0.42f;
constexpr float fixedStep = 1.0f / 120.0f;
const glm::ivec3 gridSize(80, 64, 48);
const glm::vec3 gridMin(-2.3f, -1.95f, -1.5f);
const glm::vec3 gridMax(2.3f, 2.1f, 1.5f);

void setGridBounds(const GLProgram& program) {
    glUniform3fv(program.uniformLocation("gridMin"), 1, glm::value_ptr(gridMin));
    glUniform3fv(program.uniformLocation("gridMax"), 1, glm::value_ptr(gridMax));
}

glm::vec3 cameraEye(FluidDimension dimension) {
    return dimension == FluidDimension::Two ? glm::vec3(0, 0, 7.0f) : glm::vec3(3.8f, 2.8f, 5.5f);
}

glm::mat4 cameraView(FluidDimension dimension) {
    return glm::lookAt(cameraEye(dimension), glm::vec3(0, -0.3f, 0), glm::vec3(0, 1, 0));
}

glm::mat4 cameraProjection(int width, int height) {
    return glm::perspective(glm::radians(45.0f), float(width) / height, 0.1f, 50.0f);
}
}

FluidDemo::~FluidDemo() {
    glDeleteTextures(1, &fieldTexture_);
    glDeleteVertexArrays(1, &vao_);
    glDeleteBuffers(1, &grid_);
    glDeleteBuffers(1, &density_);
    glDeleteBuffers(2, particles_);
}

bool FluidDemo::initialize(const std::filesystem::path& shaderDirectory, FluidDimension dimension) {
    dimension_ = dimension;
    if (!densityProgram_.loadCompute((shaderDirectory / "fluid_density.comp").string()) ||
        !motionProgram_.loadCompute((shaderDirectory / "fluid_motion.comp").string()) ||
        (dimension == FluidDimension::Three &&
         (!gridScatterProgram_.loadCompute((shaderDirectory / "fluid_grid_scatter.comp").string()) ||
          !gridResolveProgram_.loadCompute((shaderDirectory / "fluid_grid_resolve.comp").string()))) ||
        !particleProgram_.loadGraphics((shaderDirectory / "fluid_splat.vert").string(),
                                       (shaderDirectory / "fluid_particle.frag").string()) ||
        !compositeProgram_.loadGraphics((shaderDirectory / "fluid_composite.vert").string(),
                                        (shaderDirectory / "fluid_composite.frag").string())) return false;
    glGenBuffers(2, particles_);
    glGenBuffers(1, &density_);
    glGenVertexArrays(1, &vao_);
    if (dimension == FluidDimension::Three) {
        glGenBuffers(1, &grid_);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, grid_);
        glBufferData(GL_SHADER_STORAGE_BUFFER, gridSize.x * gridSize.y * gridSize.z * sizeof(GLuint),
                     nullptr, GL_DYNAMIC_COPY);
        glGenTextures(1, &fieldTexture_);
        glBindTexture(GL_TEXTURE_3D, fieldTexture_);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, gridSize.x, gridSize.y, gridSize.z, 0, GL_RED, GL_FLOAT, nullptr);
    }
    return reset();
}

bool FluidDemo::reset() {
    std::vector<FluidParticle> initial;
    if (dimension_ == FluidDimension::Two) {
        for (int y = 0; y < 17; ++y)
            for (int x = 0; x < 20; ++x)
                initial.push_back({glm::vec4(-1.72f + (x + (y % 2) * 0.5f) * 0.17f,
                                             -1.25f + y * 0.15f, 0.0f, 1.0f),
                                   glm::vec4(0.0f)});
    } else {
        for (int z = 0; z < 10; ++z)
            for (int y = 0; y < 10; ++y)
                for (int x = 0; x < 10; ++x)
                    initial.push_back({glm::vec4(-1.72f + x * 0.20f, -1.30f + y * 0.20f,
                                                 -0.90f + z * 0.20f, 1.0f), glm::vec4(0.0f)});
    }
    count_ = static_cast<int>(initial.size());
    current_ = 0;
    simulationTime_ = 0.0f;
    for (GLuint buffer : particles_) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, buffer);
        glBufferData(GL_SHADER_STORAGE_BUFFER, initial.size() * sizeof(FluidParticle), initial.data(), GL_DYNAMIC_COPY);
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, density_);
    glBufferData(GL_SHADER_STORAGE_BUFFER, initial.size() * sizeof(float), nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        Logger::error("Fluid buffer initialization failed (OpenGL error " + std::to_string(error) + ")");
        return false;
    }
    return true;
}

bool FluidDemo::step(float dt) {
    if (!std::isfinite(dt) || dt <= 0.0f || dt > fixedStep || count_ <= 0) {
        Logger::error("Invalid fluid simulation timestep");
        return false;
    }
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, particles_[current_]);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, density_);
    densityProgram_.use();
    glUniform1i(densityProgram_.uniformLocation("particleCount"), count_);
    glUniform1f(densityProgram_.uniformLocation("smoothingRadius"), smoothingRadius);
    glDispatchCompute((count_ + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    motionProgram_.use();
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, particles_[1 - current_]);
    glUniform1i(motionProgram_.uniformLocation("particleCount"), count_);
    glUniform1i(motionProgram_.uniformLocation("dimensions"), dimension_ == FluidDimension::Two ? 2 : 3);
    glUniform1f(motionProgram_.uniformLocation("smoothingRadius"), smoothingRadius);
    glUniform1f(motionProgram_.uniformLocation("timeStep"), dt);
    glUniform3fv(motionProgram_.uniformLocation("interactionPoint"), 1, glm::value_ptr(interactionPoint_));
    glUniform1f(motionProgram_.uniformLocation("interactionStrength"), interactionStrength_);
    glUniform1f(motionProgram_.uniformLocation("obstacleAngle"), simulationTime_ * 0.65f);
    glDispatchCompute((count_ + 63) / 64, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    current_ = 1 - current_;
    simulationTime_ += dt;
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        Logger::error("Fluid simulation failed (OpenGL error " + std::to_string(error) + ")");
        return false;
    }
    return true;
}

bool FluidDemo::render(int width, int height) {
    if (width <= 0 || height <= 0) {
        Logger::error("Invalid fluid viewport dimensions");
        return false;
    }
    const glm::vec3 eye = cameraEye(dimension_);
    const glm::mat4 view = cameraView(dimension_);
    const glm::mat4 projection = cameraProjection(width, height);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, particles_[current_]);
    if (dimension_ == FluidDimension::Three) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, grid_);
        const GLuint zero = 0;
        glClearBufferData(GL_SHADER_STORAGE_BUFFER, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, &zero);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, grid_);
        gridScatterProgram_.use();
        glUniform1i(gridScatterProgram_.uniformLocation("particleCount"), count_);
        glUniform3iv(gridScatterProgram_.uniformLocation("gridSize"), 1, glm::value_ptr(gridSize));
        glUniform1f(gridScatterProgram_.uniformLocation("particleRadius"), 0.28f);
        setGridBounds(gridScatterProgram_);
        glDispatchCompute((count_ + 63) / 64, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
        gridResolveProgram_.use();
        glUniform3iv(gridResolveProgram_.uniformLocation("gridSize"), 1, glm::value_ptr(gridSize));
        glBindImageTexture(0, fieldTexture_, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_R32F);
        glDispatchCompute((gridSize.x + 3) / 4, (gridSize.y + 3) / 4, (gridSize.z + 3) / 4);
        glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT |
                        GL_BUFFER_UPDATE_BARRIER_BIT);
    }
    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(vao_);
    compositeProgram_.use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, fieldTexture_);
    glUniform1i(compositeProgram_.uniformLocation("fieldTexture"), 0);
    glUniform1i(compositeProgram_.uniformLocation("dimensions"), dimension_ == FluidDimension::Two ? 2 : 3);
    const glm::mat4 inverseViewProjection = glm::inverse(projection * view);
    glUniformMatrix4fv(compositeProgram_.uniformLocation("inverseViewProjection"), 1, GL_FALSE,
                       glm::value_ptr(inverseViewProjection));
    glUniform3fv(compositeProgram_.uniformLocation("cameraPosition"), 1, glm::value_ptr(eye));
    glUniform1f(compositeProgram_.uniformLocation("obstacleAngle"), simulationTime_ * 0.65f);
    setGridBounds(compositeProgram_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (dimension_ == FluidDimension::Two) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        particleProgram_.use();
        glUniformMatrix4fv(particleProgram_.uniformLocation("view"), 1, GL_FALSE, glm::value_ptr(view));
        glUniformMatrix4fv(particleProgram_.uniformLocation("projection"), 1, GL_FALSE, glm::value_ptr(projection));
        glUniform1f(particleProgram_.uniformLocation("viewportHeight"), float(height));
        glUniform1f(particleProgram_.uniformLocation("radius"), 0.08f);
        glDrawArrays(GL_POINTS, 0, count_);
        glDisable(GL_BLEND);
    }
    glBindVertexArray(0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        Logger::error("Fluid rendering failed (OpenGL error " + std::to_string(error) + ")");
        return false;
    }
    return true;
}

bool FluidDemo::readParticles(std::vector<FluidParticle>& particles) const {
    particles.resize(static_cast<size_t>(count_));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, particles_[current_]);
    glGetBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, particles.size() * sizeof(FluidParticle), particles.data());
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    if (const GLenum error = glGetError(); error != GL_NO_ERROR) {
        Logger::error("Fluid readback failed (OpenGL error " + std::to_string(error) + ")");
        return false;
    }
    return true;
}

bool FluidDemo::run(GLFWwindow* window, int benchmarkFrames) {
    Logger::info("Fluid demo: left mouse pushes, right mouse pulls, Space pauses, R resets, Escape exits");
    bool paused = false, previousSpace = false, previousReset = false;
    double previous = glfwGetTime(), accumulated = 0.0;
    constexpr int warmupFrames = 10;
    int frame = 0;
    double measuredMs = 0.0;
    if (benchmarkFrames > 0) glfwSwapInterval(0);
    while (!glfwWindowShouldClose(window)) {
        const auto frameStart = std::chrono::steady_clock::now();
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) break;
        const bool space = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        const bool resetKey = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
        if (space && !previousSpace) paused = !paused;
        if (resetKey && !previousReset && !reset()) return false;
        previousSpace = space;
        previousReset = resetKey;
        const double now = glfwGetTime();
        accumulated += paused ? 0.0 : std::clamp(now - previous, 0.0, 0.05);
        previous = now;
        int width = 0, height = 0, windowWidth = 0, windowHeight = 0;
        glfwGetFramebufferSize(window, &width, &height);
        glfwGetWindowSize(window, &windowWidth, &windowHeight);
        float strength = 0.0f;
        if (width > 0 && height > 0 && windowWidth > 0 && windowHeight > 0) {
            strength = 18.0f * (int(glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) -
                                int(glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS));
            if (strength != 0.0f) {
                double mouseX = 0.0, mouseY = 0.0;
                glfwGetCursorPos(window, &mouseX, &mouseY);
                const glm::vec2 ndc(2.0f * float(mouseX) / windowWidth - 1.0f,
                                    1.0f - 2.0f * float(mouseY) / windowHeight);
                const glm::mat4 inverse = glm::inverse(cameraProjection(width, height) * cameraView(dimension_));
                glm::vec4 farPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
                const glm::vec3 eye = cameraEye(dimension_);
                const glm::vec3 direction = glm::normalize(glm::vec3(farPoint) / farPoint.w - eye);
                if (std::abs(direction.z) > 0.0001f) {
                    const float t = -eye.z / direction.z;
                    if (t > 0.0f) interactionPoint_ = eye + direction * t;
                    else strength = 0.0f;
                } else strength = 0.0f;
            }
        }
        interactionStrength_ = strength;
        if (benchmarkFrames > 0) {
            if (!step(fixedStep) || !step(fixedStep)) return false;
        } else {
            while (accumulated >= fixedStep) {
                if (!step(fixedStep)) return false;
                accumulated -= fixedStep;
            }
        }
        if (width <= 0 || height <= 0) { glfwWaitEventsTimeout(0.05); continue; }
        if (!render(width, height)) return false;
        if (benchmarkFrames > 0) {
            glFinish();
            if (frame >= warmupFrames)
                measuredMs += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - frameStart).count();
            ++frame;
        }
        glfwSwapBuffers(window);
        if (benchmarkFrames > 0 && frame == warmupFrames + benchmarkFrames) {
            const double averageMs = measuredMs / benchmarkFrames;
            Logger::info("Fluid benchmark: " + std::to_string(averageMs) + " ms/frame, " +
                         std::to_string(1000.0 / averageMs) + " FPS (" +
                         std::to_string(benchmarkFrames) + " synchronized frames)");
            break;
        }
    }
    return true;
}

} // namespace rayzen
