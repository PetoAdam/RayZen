#ifndef MATERIAL_H
#define MATERIAL_H

#include <glm/glm.hpp>

enum MaterialTexturePattern : int {
    MATERIAL_TEX_NONE   = -1,
    MATERIAL_TEX_GRASS  = 0,
    MATERIAL_TEX_DIRT   = 1,
    MATERIAL_TEX_STONE  = 2,
    MATERIAL_TEX_LOG    = 3,
    MATERIAL_TEX_LEAVES = 4
};

struct Material {
    glm::vec3 albedo;        // Base color of the material
    float metallic;          // Metallic factor (0 = dielectric, 1 = metallic)
    float roughness;         // Roughness factor (0 = smooth, 1 = rough)
    float reflectivity;      // Reflectivity factor (0 = non-reflective, 1 = fully reflective)
    float transparency;      // Transparency factor (0 = opaque, 1 = fully transparent)
    float ior;               // Index of Refraction (for refraction)
    int texturePattern;      // Procedural texture selector for shaders
    float padding;           // Align to 16 bytes for SSBOs

    Material(const glm::vec3& albedo, float metallic, float roughness,
             float reflectivity = 0.0f, float transparency = 0.0f, float ior = 1.5f,
             int texturePattern = MATERIAL_TEX_NONE)
        : albedo(albedo), metallic(metallic), roughness(roughness),
          reflectivity(reflectivity), transparency(transparency), ior(ior),
          texturePattern(texturePattern), padding(0.0f) {}
};

#endif
