#ifndef RAY_H
#define RAY_H

#include <glm/glm.hpp>

class Ray {
public:
    glm::vec3 origin;
    glm::vec3 direction;

    Ray() {}
    Ray(const glm::vec3& origin, const glm::vec3& direction)
        : origin(origin), direction(glm::normalize(direction)) {}

    glm::vec3 pointAt(float t) const {
        return origin + t * direction;
    }
};

#endif
