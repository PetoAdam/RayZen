#ifndef MATERIAL_H
#define MATERIAL_H

#include <cstddef>
#include <glm/glm.hpp>

struct Material {
    glm::vec3 albedo;        // Base color of the material
    float metallic;          // Metallic factor (0 = dielectric, 1 = metallic)
    float roughness;         // Roughness factor (0 = smooth, 1 = rough)
    float reflectivity;      // Dielectric F0 boost: 0 = IOR-derived, 1 = twice IOR-derived.
    float transparency;      // Transparency factor (0 = opaque, 1 = fully transparent)
    float ior;               // Index of Refraction (for refraction)
    float subsurfaceMeanFreePath; // World units; zero disables subsurface scattering.
    int baseColorTexture = -1; // Index into Scene::textures; -1 means no map.
    float padding[2]{};

    Material(const glm::vec3& albedo, float metallic, float roughness, 
             float reflectivity = 0.0f, float transparency = 0.0f, float ior = 1.5f,
             float subsurfaceMeanFreePath = 0.0f)
        : albedo(albedo), metallic(metallic), roughness(roughness), 
          reflectivity(reflectivity), transparency(transparency), ior(ior),
          subsurfaceMeanFreePath(subsurfaceMeanFreePath) {}
};

static_assert(offsetof(Material, subsurfaceMeanFreePath) == 32 && sizeof(Material) == 48,
              "Material must match the GLSL std430 layout");

#endif
