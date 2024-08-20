#ifndef SPHERE_H
#define SPHERE_H

#include "Ray.h"
#include "Material.h"
#include "HitRecord.h"

class Sphere {
public:
    glm::vec3 center;
    float radius;
    Material* material;

    Sphere(const glm::vec3& cen, float r, Material* mat) : center(cen), radius(r), material(mat) {}

    bool hit(const Ray& r, float t_min, float t_max, HitRecord& rec) const {
        glm::vec3 oc = r.origin - center;
        float a = glm::dot(r.direction, r.direction);
        float b = 2.0f * glm::dot(oc, r.direction);
        float c = glm::dot(oc, oc) - radius * radius;
        float discriminant = b * b - 4 * a * c;
        if (discriminant > 0) {
            float temp = (-b - sqrt(discriminant)) / (2.0f * a);
            if (temp < t_max && temp > t_min) {
                rec.t = temp;
                rec.p = r.origin + temp * r.direction;
                rec.normal = (rec.p - center) / radius;
                rec.material = material;
                return true;
            }
        }
        return false;
    }
};

#endif
