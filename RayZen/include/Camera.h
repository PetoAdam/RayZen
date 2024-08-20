#ifndef CAMERA_H
#define CAMERA_H

#include <glm/glm.hpp>
#include "Ray.h"

class Camera {
public:
    glm::vec3 origin;
    glm::vec3 lower_left_corner;
    glm::vec3 horizontal;
    glm::vec3 vertical;
    glm::vec3 u, v, w;
    float zoom;

    Camera() {
        origin = glm::vec3(0.0f, 0.0f, 0.0f);
        lower_left_corner = glm::vec3(-2.0f, -1.0f, -1.0f);
        horizontal = glm::vec3(4.0f, 0.0f, 0.0f);
        vertical = glm::vec3(0.0f, 2.0f, 0.0f);
        zoom = 1.0f;
        updateCameraVectors();
    }

    Ray getRay(float u, float v) const {
        return Ray(origin, lower_left_corner + u * horizontal + v * vertical - origin);
    }

    void processMouseMovement(float xoffset, float yoffset) {
        // Adjust the camera direction based on mouse movement
        // Example implementation; adjust as needed
    }

    void updateCameraVectors() {
        w = glm::normalize(origin - lower_left_corner);
        u = glm::normalize(glm::cross(glm::vec3(0.0f, 1.0f, 0.0f), w));
        v = glm::cross(w, u);
        horizontal = u * 4.0f * zoom;
        vertical = v * 2.0f * zoom;
        lower_left_corner = origin - horizontal / 2.0f - vertical / 2.0f - w;
    }
};

#endif
