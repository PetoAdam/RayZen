#ifndef SCENE_H
#define SCENE_H

#include <vector>
#include "Ray.h"
#include "Mesh.h"
#include "Light.h"
#include "HitRecord.h"

class Scene {
public:
    std::vector<Mesh> objects;
    std::vector<Light> lights;

    void addObject(const Mesh& obj) {
        objects.push_back(obj);
    }

    void addLight(const Light& light) {
        lights.push_back(light);
    }
};

#endif
