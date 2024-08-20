#ifndef SPHERE_H
#define SPHERE_H

#include <glm/glm.hpp>

struct Sphere {
    glm::vec3 center;
    float radius;
    int materialIndex;

    Sphere(const glm::vec3& center, float radius, int materialIndex)
        : center(center), radius(radius), materialIndex(materialIndex) {}
};

#endif
