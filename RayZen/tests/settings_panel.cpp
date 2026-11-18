#include <iostream>

#include "Renderer/SettingsPanel.h"
#include <GLFW/glfw3.h>

namespace {
int activeKey = -1;
}

extern "C" int glfwGetKey(GLFWwindow*, int key) {
    return key == activeKey ? GLFW_PRESS : GLFW_RELEASE;
}

int main() {
    Scene scene;
    scene.materials.emplace_back(glm::vec3(1), 0.0f, 0.5f);
    scene.materials.emplace_back(glm::vec3(1), 0.0f, 0.5f);
    scene.textures.resize(2);
    rayzen::RenderConfig config;
    rayzen::SettingsPanel panel;
    const auto press = [&](int key) {
        activeKey = key;
        const bool changed = panel.update(nullptr, config, scene);
        activeKey = -1;
        panel.update(nullptr, config, scene);
        return changed;
    };
    press(GLFW_KEY_F3);
    press(GLFW_KEY_TAB);
    press(GLFW_KEY_RIGHT);
    for (int i = 0; i < 9; ++i) press(GLFW_KEY_DOWN);
    if (!press(GLFW_KEY_RIGHT) || scene.materials[1].subsurfaceMeanFreePath < 0.049f ||
        scene.materials[0].subsurfaceMeanFreePath != 0.0f || config.subsurfaceEnabled) {
        std::cerr << "Material-specific scattering edit failed\n";
        return 1;
    }
    press(GLFW_KEY_DOWN);
    if (!press(GLFW_KEY_RIGHT) || scene.materials[1].baseColorTexture != 0 ||
        !press(GLFW_KEY_RIGHT) || scene.materials[1].baseColorTexture != 1 ||
        !press(GLFW_KEY_RIGHT) || scene.materials[1].baseColorTexture != -1) {
        std::cerr << "Material texture selection failed\n";
        return 1;
    }
    press(GLFW_KEY_F4);
    if (!panel.consumeSaveRequested() || panel.consumeSaveRequested()) {
        std::cerr << "Material save shortcut failed\n";
        return 1;
    }
    press(GLFW_KEY_TAB);
    for (int i = 0; i < 8; ++i) press(GLFW_KEY_DOWN);
    if (!press(GLFW_KEY_RIGHT) || !config.subsurfaceEnabled) {
        std::cerr << "Subsurface quality switch failed\n";
        return 1;
    }
    press(GLFW_KEY_DOWN);
    if (!press(GLFW_KEY_RIGHT) || !config.causticsEnabled) {
        std::cerr << "Caustics switch failed\n";
        return 1;
    }
    press(GLFW_KEY_DOWN);
    if (!press(GLFW_KEY_LEFT) || config.causticPhotonCount != 32768) {
        std::cerr << "Caustic photon budget edit failed\n";
        return 1;
    }
    return 0;
}
