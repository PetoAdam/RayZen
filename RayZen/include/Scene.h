#ifndef SCENE_H
#define SCENE_H

#include <vector>
#include "Ray.h"
#include "Sphere.h"
#include "Light.h"

class Scene {
public:
    std::vector<Sphere> objects;
    std::vector<Light> lights;

    void addObject(const Sphere& obj) {
        objects.push_back(obj);
    }

    void addLight(const Light& light) {
        lights.push_back(light);
    }

    bool hit(const Ray& ray, float t_min, float t_max, HitRecord& rec) const {
        bool hit_anything = false;
        float closest_so_far = t_max;

        for (const auto& object : objects) {
            if (object.hit(ray, t_min, closest_so_far, rec)) {
                hit_anything = true;
                closest_so_far = rec.t;
            }
        }
        return hit_anything;
    }
};

#endif
