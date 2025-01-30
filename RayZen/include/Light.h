#ifndef LIGHT_H
#define LIGHT_H

#include <glm/glm.hpp>

class Light {
public:
    glm::vec4 positionOrDirection;  // Position for point lights, Direction for directional lights
    glm::vec3 color;  // Light color
    float power;    // Power

    Light(const glm::vec4& positionOrDirection, const glm::vec3& color, const float power)
        : positionOrDirection(positionOrDirection), color(color), power(power) {}

    bool isPointLight() const {
        return positionOrDirection.w == 1.0f; // If w is 1.0, it's a point light
    }

    glm::vec3 getPosition() const {
        return glm::vec3(positionOrDirection.x, positionOrDirection.y, positionOrDirection.z);  // Extract the position part (for point lights)
    }

    glm::vec3 getDirection() const {
        return isPointLight() ? glm::vec3(0.0f) : glm::normalize(glm::vec3(positionOrDirection.x, positionOrDirection.y, positionOrDirection.z)); // Direction part (for directional lights)
    }

    glm::vec3 getColor() const {
        return color;
    }
};

#endif
