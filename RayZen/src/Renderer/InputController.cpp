#include "Renderer/InputController.h"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "Logger.h"

namespace rayzen {

namespace {

// Returns the picked (gameObjectIndex, triIndex) under the cursor, or (-1,-1) if no hit.
// Uses brute-force ray/triangle intersection in mesh-local space.
std::pair<int, int> pickBLAS(GLFWwindow* window, int viewportW, int viewportH,
                             const Camera& camera, const Scene& scene) {
    double mouseX = 0.0, mouseY = 0.0;
    glfwGetCursorPos(window, &mouseX, &mouseY);
    const float ndcX = 2.0f * float(mouseX) / float(viewportW) - 1.0f;
    const float ndcY = 1.0f - 2.0f * float(mouseY) / float(viewportH);

    glm::vec4 rayClip(ndcX, ndcY, -1.0f, 1.0f);
    glm::mat4 invProj = glm::inverse(camera.projectionMatrix);
    glm::mat4 invView = glm::inverse(camera.viewMatrix);
    glm::vec4 rayEye = invProj * rayClip;
    rayEye = glm::vec4(rayEye.x, rayEye.y, -1.0f, 0.0f);
    glm::vec3 rayDir = glm::normalize(glm::vec3(invView * rayEye));
    glm::vec3 rayOrigin = camera.position;

    float closestT = 1e30f;
    int pickedBLAS = -1, pickedTri = -1;
    for (size_t objIdx = 0; objIdx < scene.gameObjects.size(); ++objIdx) {
        const auto& obj = scene.gameObjects[objIdx];
        const auto& mesh = obj.mesh;
        if (!mesh) continue;
        glm::mat4 invTransform = glm::inverse(obj.transform);
        glm::vec3 localOrigin = glm::vec3(invTransform * glm::vec4(rayOrigin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(invTransform * glm::vec4(rayDir, 0.0f)));
        for (size_t triIdx = 0; triIdx < mesh->triangles.size(); ++triIdx) {
            const auto& tri = mesh->triangles[triIdx];
            glm::vec3 edge1 = tri.v1 - tri.v0;
            glm::vec3 edge2 = tri.v2 - tri.v0;
            glm::vec3 h = glm::cross(localDir, edge2);
            float a = glm::dot(edge1, h);
            if (std::abs(a) < 1e-6f) continue;
            float f = 1.0f / a;
            glm::vec3 s = localOrigin - tri.v0;
            float u = f * glm::dot(s, h);
            if (u < 0.0f || u > 1.0f) continue;
            glm::vec3 q = glm::cross(s, edge1);
            float v = f * glm::dot(localDir, q);
            if (v < 0.0f || u + v > 1.0f) continue;
            float t = f * glm::dot(edge2, q);
            if (t > 0.0001f && t < closestT) {
                closestT = t;
                pickedBLAS = static_cast<int>(objIdx);
                pickedTri = static_cast<int>(triIdx);
            }
        }
    }
    return {pickedBLAS, pickedTri};
}

} // namespace

FrameInputResult InputController::update(GLFWwindow* window,
                                          float deltaTime,
                                          int viewportW,
                                          int viewportH,
                                          Camera& camera,
                                          DebugState& debug,
                                          const Scene& scene) {
    FrameInputResult out;

    // Snapshot camera state to detect motion.
    const glm::vec3 prevPos = camera.position;
    const glm::vec3 prevTarget = camera.target;

    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, true);
    }
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.moveForward(deltaTime);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.moveBackward(deltaTime);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.moveLeft(deltaTime);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.moveRight(deltaTime);

    // Mouse drag rotation (left button held).
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
        if (!dragging_) {
            glfwGetCursorPos(window, &lastMouseX_, &lastMouseY_);
            dragging_ = true;
        }
        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(window, &mouseX, &mouseY);
        const double offsetX = mouseX - lastMouseX_;
        const double offsetY = lastMouseY_ - mouseY;
        camera.rotate(static_cast<float>(offsetX), static_cast<float>(offsetY));
        lastMouseX_ = mouseX;
        lastMouseY_ = mouseY;
    } else {
        dragging_ = false;
    }

    // F1: editor mode toggle.
    if (glfwGetKey(window, GLFW_KEY_F1) == GLFW_PRESS) {
        if (!editorKeyPressed_) {
            out.requestEditorToggle = true;
            editorKeyPressed_ = true;
        }
    } else {
        editorKeyPressed_ = false;
    }
    // F2: swap interval toggle.
    if (glfwGetKey(window, GLFW_KEY_F2) == GLFW_PRESS) {
        if (!swapKeyPressed_) {
            out.requestSwapToggle = true;
            swapKeyPressed_ = true;
        }
    } else {
        swapKeyPressed_ = false;
    }
    // L: light markers.
    if (glfwGetKey(window, GLFW_KEY_L) == GLFW_PRESS) {
        if (!lKeyPressed_) {
            debug.showLights = !debug.showLights;
            lKeyPressed_ = true;
            Logger::info(std::string("Light debugging: ") + (debug.showLights ? "On" : "Off"));
        }
    } else {
        lKeyPressed_ = false;
    }
    // B: BVH wireframe.
    if (glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS) {
        if (!bKeyPressed_) {
            debug.showBVH = !debug.showBVH;
            bKeyPressed_ = true;
            Logger::info(std::string("BVH wireframe debugging: ") + (debug.showBVH ? "On" : "Off"));
        }
    } else {
        bKeyPressed_ = false;
    }
    // N: BVH mode (TLAS/BLAS).
    if (glfwGetKey(window, GLFW_KEY_N) == GLFW_PRESS) {
        if (!nKeyPressed_) {
            debug.bvhMode = (debug.bvhMode + 1) % 2;
            nKeyPressed_ = true;
            Logger::info(std::string("BVH debug mode: ") + (debug.bvhMode == 0 ? "TLAS" : "BLAS"));
        }
    } else {
        nKeyPressed_ = false;
    }

    // Mouse picking for BLAS debug mode.
    if (debug.showBVH && debug.bvhMode == 1) {
        auto [blas, tri] = pickBLAS(window, viewportW, viewportH, camera, scene);
        if (blas >= 0 && tri >= 0) {
            debug.selectedBLAS = blas;
            debug.selectedTri = tri;
        }
    }

    // Detect any camera motion.
    if (prevPos != camera.position || prevTarget != camera.target) {
        out.cameraMoved = true;
    }
    return out;
}

} // namespace rayzen
