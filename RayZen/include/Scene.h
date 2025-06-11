#ifndef SCENE_H
#define SCENE_H

#include <vector>
#include "Camera.h"
#include "Material.h"
#include "Light.h"
#include "GameObject.h"

// The Scene class groups together the camera, lights, materials, and game objects.
class Scene {
public:
    Camera camera;
    std::vector<Material> materials;
    std::vector<Light> lights;
    std::vector<GameObject> gameObjects;

    Scene() {};
};

#endif
