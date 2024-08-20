#include "Material.h"

glm::vec3 Material::scatter(const Ray& ray_in, const HitRecord& rec, glm::vec3& attenuation) const {
    switch (type) {
        case MaterialType::Lambertian: {
            glm::vec3 target = rec.p + rec.normal + random_in_unit_sphere();
            Ray scattered(rec.p, target - rec.p);
            attenuation = albedo;
            return glm::vec3(1.0f);
        }
        case MaterialType::Phong: {
            glm::vec3 view_dir = glm::normalize(-ray_in.direction);
            glm::vec3 reflect_dir = glm::reflect(glm::normalize(ray_in.direction), rec.normal);
            float spec = glm::pow(glm::max(glm::dot(view_dir, reflect_dir), 0.0f), shininess);
            attenuation = diffuse + specular * spec;
            return glm::vec3(1.0f);
        }
        case MaterialType::Metallic: {
            glm::vec3 reflected = glm::reflect(glm::normalize(ray_in.direction), rec.normal);
            Ray scattered(rec.p, reflected + fuzz * random_in_unit_sphere());
            attenuation = albedo;
            return glm::dot(scattered.direction, rec.normal) > 0 ? glm::vec3(1.0f) : glm::vec3(0.0f);
        }
        case MaterialType::Subsurface: {
            // Simplified SSS model
            glm::vec3 scattered = rec.p + rec.normal * sss_scale;
            Ray new_ray(rec.p, scattered - rec.p);
            attenuation = albedo;
            return glm::vec3(1.0f);
        }
        default:
            return glm::vec3(0.0f);
    }
}
