#ifndef MATERIAL_H
#define MATERIAL_H

#include <glm/glm.hpp>

struct Material {
    glm::vec3 diffuse;
    glm::vec3 specular;
    float shininess;
    float metallic;
    float fuzz;
    glm::vec3 albedo;
    float reflectivity;
    float transparency;
    float ior;

    Material(const glm::vec3& diffuse, const glm::vec3& specular, float shininess, float metallic, 
             float fuzz, const glm::vec3& albedo, float reflectivity = 0.0f, 
             float transparency = 0.0f, float ior = 1.5f)
        : diffuse(diffuse), specular(specular), shininess(shininess), metallic(metallic), 
          fuzz(fuzz), albedo(albedo), reflectivity(reflectivity), transparency(transparency), ior(ior) {}
};

#endif
