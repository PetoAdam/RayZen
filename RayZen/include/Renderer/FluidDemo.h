#pragma once

#include <cstddef>
#include <filesystem>
#include <vector>

#include <GL/glew.h>
#include <glm/glm.hpp>

#include "Renderer/GLResources.h"

struct GLFWwindow;

namespace rayzen {

enum class FluidDimension { Two, Three };

struct alignas(16) FluidParticle {
    glm::vec4 position;
    glm::vec4 velocity;
};
static_assert(sizeof(FluidParticle) == 32 && offsetof(FluidParticle, velocity) == 16,
              "FluidParticle must match the GLSL std430 layout");

class FluidDemo {
public:
    FluidDemo() = default;
    ~FluidDemo();
    FluidDemo(const FluidDemo&) = delete;
    FluidDemo& operator=(const FluidDemo&) = delete;

    bool initialize(const std::filesystem::path& shaderDirectory, FluidDimension dimension);
    bool step(float dt);
    bool render(int width, int height);
    bool run(GLFWwindow* window, int benchmarkFrames = 0);
    bool reset();
    bool readParticles(std::vector<FluidParticle>& particles) const;
    void setInteraction(const glm::vec3& point, float strength) {
        interactionPoint_ = point;
        interactionStrength_ = strength;
    }

private:
    FluidDimension dimension_ = FluidDimension::Two;
    GLProgram densityProgram_;
    GLProgram motionProgram_;
    GLProgram gridScatterProgram_;
    GLProgram gridResolveProgram_;
    GLProgram particleProgram_;
    GLProgram compositeProgram_;
    GLuint particles_[2]{};
    GLuint density_ = 0;
    GLuint vao_ = 0;
    GLuint grid_ = 0;
    GLuint fieldTexture_ = 0;
    int current_ = 0;
    int count_ = 0;
    glm::vec3 interactionPoint_{0.0f};
    float interactionStrength_ = 0.0f;
    float simulationTime_ = 0.0f;
};

} // namespace rayzen
