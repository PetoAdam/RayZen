#pragma once
#include <glm/mat4x4.hpp>
#include <memory>
#include "Mesh.h"

struct GameObject {
    std::shared_ptr<Mesh> mesh;
    glm::mat4 transform = glm::mat4(1.0f);
    // Optionally, a name or ID if needed
};