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

    float fov;  // Field of view
    float aspectRatio;
    float nearClip;
    float farClip;

    Camera(glm::vec3 position, glm::vec3 target, glm::vec3 up, float fov, float aspectRatio, float nearClip, float farClip)
        : position(position), target(target), up(up), fov(fov), aspectRatio(aspectRatio), nearClip(nearClip), farClip(farClip) {
        updateViewMatrix();
        updateProjectionMatrix();
    }

    void updateViewMatrix() {
        viewMatrix = glm::lookAt(position, target, up);
    }

    void updateProjectionMatrix() {
        projectionMatrix = glm::perspective(glm::radians(fov), aspectRatio, nearClip, farClip);
    }

    void sendToShader(GLuint shaderProgram) const {
        glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.viewMatrix"), 1, GL_FALSE, &viewMatrix[0][0]);
        glUniformMatrix4fv(glGetUniformLocation(shaderProgram, "camera.projectionMatrix"), 1, GL_FALSE, &projectionMatrix[0][0]);
        glUniform3fv(glGetUniformLocation(shaderProgram, "camera.position"), 1, &position[0]);
    }

    void processMouseMovement(float xoffset, float yoffset) {
        // Example: simple yaw-pitch camera rotation (you can expand this)
        glm::vec3 front = glm::normalize(target - position);
        glm::vec3 right = glm::normalize(glm::cross(front, up));
        //glm::vec3 rotatedFront = glm::rotate(front, glm::radians(xoffset), up);
        //rotatedFront = glm::rotate(rotatedFront, glm::radians(yoffset), right);
        //target = position + rotatedFront;
        updateViewMatrix();
    }
};

#endif
