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

    Material(const glm::vec3& diffuse, const glm::vec3& specular, float shininess, float metallic, float fuzz, const glm::vec3& albedo)
        : diffuse(diffuse), specular(specular), shininess(shininess), metallic(metallic), fuzz(fuzz), albedo(albedo) {}
};

#endif
