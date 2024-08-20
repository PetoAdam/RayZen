#ifndef MATERIAL_H
#define MATERIAL_H

#include <glm/glm.hpp>
#include "Ray.h"
#include "HitRecord.h"

enum class MaterialType {
    Lambertian,
    Phong,
    Metallic,
    Subsurface
};

class Material {
public:
    Material(MaterialType type) : type(type) {}

    void setDiffuse(const glm::vec3& diffuse) { this->diffuse = diffuse; }
    void setSpecular(const glm::vec3& specular) { this->specular = specular; }
    void setShininess(float shininess) { this->shininess = shininess; }
    void setMetallic(float metallic) { this->metallic = metallic; }
    void setFuzz(float fuzz) { this->fuzz = fuzz; }
    void setSSSScale(float scale) { this->sss_scale = scale; }
    void setAlbedo(const glm::vec3& albedo) { this->albedo = albedo; }

    glm::vec3 scatter(const Ray& ray_in, const HitRecord& rec, glm::vec3& attenuation) const;

private:
    MaterialType type;
    glm::vec3 diffuse;
    glm::vec3 specular;
    float shininess = 32.0f;
    float metallic = 0.0f;
    float fuzz = 0.0f;
    float sss_scale = 0.0f;
    glm::vec3 albedo;

    glm::vec3 random_in_unit_sphere() const {
        glm::vec3 p;
        do {
            p = 2.0f * glm::vec3(drand48(), drand48(), drand48()) - glm::vec3(1, 1, 1);
        } while (glm::dot(p, p) >= 1.0f);
        return p;
    }
};

#endif
