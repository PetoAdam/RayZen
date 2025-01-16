#ifndef CAMERA_H
#define CAMERA_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

class Camera {
public:
    glm::vec3 position;
    glm::vec3 target;
    glm::vec3 up;

    glm::mat4 viewMatrix;
    glm::mat4 projectionMatrix;

    float fov;
    float aspectRatio;
    float nearClip;
    float farClip;

    float speed;
    float sensitivity;

    float yaw;
    float pitch;

    Camera(glm::vec3 position, glm::vec3 target, glm::vec3 up, float fov, float aspectRatio, float nearClip, float farClip)
        : position(position), target(target), up(up), fov(fov), aspectRatio(aspectRatio), nearClip(nearClip), farClip(farClip), speed(1.f), sensitivity(0.1f), yaw(-90.0f), pitch(0.0f) {
        updateViewMatrix();
        updateProjectionMatrix();
    }

    void updateViewMatrix() {
        viewMatrix = glm::lookAt(position, position + target, up);
    }

    void updateProjectionMatrix() {
        projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip);
    }

    void moveForward(float deltaTime) {
        position += speed * deltaTime * target;  // Move in the direction the camera is facing
    }

    void moveBackward(float deltaTime) {
        position -= speed * deltaTime * target;  // Move opposite to the camera's facing direction
    }

    void moveLeft(float deltaTime) {
        position -= glm::normalize(glm::cross(target, up)) * speed * deltaTime;  // Move left relative to the camera's orientation
    }

    void moveRight(float deltaTime) {
        position += glm::normalize(glm::cross(target, up)) * speed * deltaTime;  // Move right relative to the camera's orientation
    }

    void rotate(float offsetX, float offsetY) {
        yaw += offsetX * sensitivity;
        pitch += offsetY * sensitivity;

        if (pitch > 89.0f) pitch = 89.0f;  // Prevent the camera from flipping
        if (pitch < -89.0f) pitch = -89.0f;

        // Recalculate the target direction using yaw and pitch
        glm::vec3 direction;
        direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
        direction.y = sin(glm::radians(pitch));
        direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
        target = glm::normalize(direction);

        // Update the right and up vectors
        glm::vec3 right = glm::normalize(glm::cross(target, glm::vec3(0.0f, 1.0f, 0.0f)));
        up = glm::normalize(glm::cross(right, target));

        updateViewMatrix();  // Update the view matrix to reflect the new target direction
    }
};


#endif
