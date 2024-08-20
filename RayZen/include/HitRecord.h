#ifndef HITRECORD_H
#define HITRECORD_H

#include <glm/glm.hpp>
#include "Material.h"

class Material;

struct HitRecord {
    glm::vec3 p;        // Intersection point
    glm::vec3 normal;   // Normal at the intersection
    Material* material; // Material of the intersected object
    float t;            // Distance from the ray origin to the intersection
};

#endif
