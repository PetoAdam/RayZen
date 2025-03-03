#ifndef MATERIAL_H
#define MATERIAL_H

#include <glm/glm.hpp>

struct Material {
    glm::vec3 albedo;        // Base color of the material
    float metallic;          // Metallic factor (0 = dielectric, 1 = metallic)
    float roughness;         // Roughness factor (0 = smooth, 1 = rough)
    float reflectivity;      // Reflectivity factor (0 = non-reflective, 1 = fully reflective)
    float transparency;      // Transparency factor (0 = opaque, 1 = fully transparent)
    float ior;               // Index of Refraction (for refraction)

    Material(const glm::vec3& albedo, float metallic, float roughness, 
             float reflectivity = 0.0f, float transparency = 0.0f, float ior = 1.5f)
        : albedo(albedo), metallic(metallic), roughness(roughness), 
          reflectivity(reflectivity), transparency(transparency), ior(ior) {}
} __attribute__((std430));

#endif
