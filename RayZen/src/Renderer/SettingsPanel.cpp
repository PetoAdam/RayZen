#include "Renderer/SettingsPanel.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
#include <sstream>
#include <vector>

#include <GLFW/glfw3.h>

namespace rayzen {
namespace {

using Glyph = std::array<unsigned char, 7>;

Glyph glyph(char c) {
    switch (c) {
    case 'A': return {14,17,17,31,17,17,17};
    case 'B': return {30,17,17,30,17,17,30};
    case 'C': return {14,17,16,16,16,17,14};
    case 'D': return {30,17,17,17,17,17,30};
    case 'E': return {31,16,16,30,16,16,31};
    case 'F': return {31,16,16,30,16,16,16};
    case 'G': return {14,17,16,23,17,17,15};
    case 'H': return {17,17,17,31,17,17,17};
    case 'I': return {14,4,4,4,4,4,14};
    case 'J': return {7,2,2,2,18,18,12};
    case 'K': return {17,18,20,24,20,18,17};
    case 'L': return {16,16,16,16,16,16,31};
    case 'M': return {17,27,21,21,17,17,17};
    case 'N': return {17,25,21,19,17,17,17};
    case 'O': return {14,17,17,17,17,17,14};
    case 'P': return {30,17,17,30,16,16,16};
    case 'Q': return {14,17,17,17,21,18,13};
    case 'R': return {30,17,17,30,20,18,17};
    case 'S': return {15,16,16,14,1,1,30};
    case 'T': return {31,4,4,4,4,4,4};
    case 'U': return {17,17,17,17,17,17,14};
    case 'V': return {17,17,17,17,17,10,4};
    case 'W': return {17,17,17,21,21,21,10};
    case 'X': return {17,17,10,4,10,17,17};
    case 'Y': return {17,17,10,4,4,4,4};
    case 'Z': return {31,1,2,4,8,16,31};
    case '0': return {14,17,19,21,25,17,14};
    case '1': return {4,12,4,4,4,4,14};
    case '2': return {14,17,1,2,4,8,31};
    case '3': return {30,1,1,14,1,1,30};
    case '4': return {2,6,10,18,31,2,2};
    case '5': return {31,16,16,30,1,1,30};
    case '6': return {14,16,16,30,17,17,14};
    case '7': return {31,1,2,4,8,8,8};
    case '8': return {14,17,17,14,17,17,14};
    case '9': return {14,17,17,15,1,1,14};
    case ':': return {0,4,4,0,4,4,0};
    case '.': return {0,0,0,0,0,4,4};
    case '-': return {0,0,0,31,0,0,0};
    case '>': return {16,8,4,2,4,8,16};
    default: return {};
    }
}

struct Vertex {
    float x, y, r, g, b;
};

void rectangle(std::vector<Vertex>& vertices, float x, float y, float w, float h,
               int width, int height, float r, float g, float b) {
    const float left = 2.0f * x / width - 1.0f;
    const float right = 2.0f * (x + w) / width - 1.0f;
    const float top = 1.0f - 2.0f * y / height;
    const float bottom = 1.0f - 2.0f * (y + h) / height;
    vertices.insert(vertices.end(), {
        {left, top, r, g, b}, {left, bottom, r, g, b}, {right, bottom, r, g, b},
        {left, top, r, g, b}, {right, bottom, r, g, b}, {right, top, r, g, b}
    });
}

void text(std::vector<Vertex>& vertices, const std::string& value, float x, float y,
          int width, int height, float r, float g, float b) {
    constexpr float scale = 2.0f;
    for (char c : value) {
        const Glyph rows = glyph(c);
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (rows[row] & (1 << (4 - col)))
                    rectangle(vertices, x + col * scale, y + row * scale, scale, scale,
                              width, height, r, g, b);
        x += 6 * scale;
    }
}

} // namespace

SettingsPanel::~SettingsPanel() {
    if (vbo_) glDeleteBuffers(1, &vbo_);
    if (vao_) glDeleteVertexArrays(1, &vao_);
}

bool SettingsPanel::initialize(const std::string& vertexPath, const std::string& fragmentPath) {
    if (!program_.loadGraphics(vertexPath, fragmentPath)) return false;
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          reinterpret_cast<void*>(offsetof(Vertex, r)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return true;
}

bool SettingsPanel::update(GLFWwindow* window, RenderConfig& config, Scene& scene) {
    const int codes[] = {GLFW_KEY_F3, GLFW_KEY_UP, GLFW_KEY_DOWN, GLFW_KEY_LEFT, GLFW_KEY_RIGHT, GLFW_KEY_TAB, GLFW_KEY_F4};
    bool pressed[7]{};
    for (int i = 0; i < 7; ++i) {
        const bool down = glfwGetKey(window, codes[i]) == GLFW_PRESS;
        pressed[i] = down && !keys_[i];
        keys_[i] = down;
    }
    if (pressed[0]) visible_ = !visible_;
    if (!visible_) return false;
    if (pressed[5]) materialPage_ = !materialPage_;
    if (pressed[6] && materialPage_) saveRequested_ = true;
    const int direction = int(pressed[4]) - int(pressed[3]);
    if (materialPage_) {
        constexpr int count = 11;
        if (pressed[1]) materialSelected_ = (materialSelected_ + count - 1) % count;
        if (pressed[2]) materialSelected_ = (materialSelected_ + 1) % count;
        if (!direction || scene.materials.empty()) return false;
        materialIndex_ = std::min(materialIndex_, scene.materials.size() - 1);
        if (materialSelected_ == 0) {
            const int size = static_cast<int>(scene.materials.size());
            materialIndex_ = static_cast<size_t>((static_cast<int>(materialIndex_) + direction + size) % size);
            return false;
        }
        Material& material = scene.materials[materialIndex_];
        switch (materialSelected_) {
        case 1: case 2: case 3: {
            float& component = material.albedo[materialSelected_ - 1];
            component = std::clamp(component + direction * 0.05f, 0.0f, 1.0f);
            break;
        }
        case 4: material.metallic = std::clamp(material.metallic + direction * 0.05f, 0.0f, 1.0f); break;
        case 5: material.roughness = std::clamp(material.roughness + direction * 0.05f, 0.0f, 1.0f); break;
        case 6: material.reflectivity = std::clamp(material.reflectivity + direction * 0.05f, 0.0f, 1.0f); break;
        case 7: material.transparency = std::clamp(material.transparency + direction * 0.05f, 0.0f, 1.0f); break;
        case 8: material.ior = std::clamp(material.ior + direction * 0.05f, 1.0f, 3.0f); break;
        case 9: material.subsurfaceMeanFreePath = std::clamp(material.subsurfaceMeanFreePath + direction * 0.05f, 0.0f, 10.0f); break;
        case 10: {
            const int countTextures = static_cast<int>(scene.textures.size()) + 1;
            material.baseColorTexture = (material.baseColorTexture + 1 + direction + countTextures) % countTextures - 1;
            break;
        }
        }
        return true;
    }
    constexpr int settingCount = 11;
    if (pressed[1]) selected_ = (selected_ + settingCount - 1) % settingCount;
    if (pressed[2]) selected_ = (selected_ + 1) % settingCount;
    if (!direction) return false;

    switch (selected_) {
    case 0:
        config.shadingMode = static_cast<ShadingMode>((static_cast<int>(config.shadingMode) + direction + 3) % 3);
        break;
    case 1:
        config.pathTracerBounceBudget = std::clamp(config.pathTracerBounceBudget + direction, 1, 100);
        break;
    case 2:
        config.accumulationEnabled = !config.accumulationEnabled;
        break;
    case 3:
        config.displayExposure = std::clamp(config.displayExposure * (direction > 0 ? 1.1f : 1.0f / 1.1f), 0.05f, 16.0f);
        break;
    case 4:
        config.pathTracerAmbient = std::clamp(config.pathTracerAmbient + direction * 0.01f, 0.0f, 1.0f);
        break;
    case 5:
        config.rasterAmbient = std::clamp(config.rasterAmbient + direction * 0.01f, 0.0f, 1.0f);
        break;
    case 6:
        config.pointLightPower = std::max(0.0f, config.pointLightPower + direction * 10.0f);
        break;
    case 7:
        config.directionalLightPower = std::max(0.0f, config.directionalLightPower + direction * 0.25f);
        break;
    case 8:
        config.subsurfaceEnabled = !config.subsurfaceEnabled;
        break;
    case 9:
        config.causticsEnabled = !config.causticsEnabled;
        break;
    case 10:
        config.causticPhotonCount = std::clamp(
            direction > 0 ? config.causticPhotonCount * 2 : config.causticPhotonCount / 2,
            1024, 262144);
        break;
    }
    return true;
}

bool SettingsPanel::consumeSaveRequested() noexcept {
    const bool requested = saveRequested_;
    saveRequested_ = false;
    return requested;
}

void SettingsPanel::render(int width, int height, const RenderConfig& config, const Scene& scene, float fps, bool editorPreview) {
    if (width <= 0 || height <= 0) return;
    const auto draw = [&](const std::vector<Vertex>& vertices) {
        glDisable(GL_DEPTH_TEST);
        program_.use();
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    };
    if (!visible_) {
        if (editorPreview) {
            std::vector<Vertex> vertices;
            rectangle(vertices, 8, 8, 190, 28, width, height, 0.04f, 0.06f, 0.10f);
            text(vertices, "EDITOR PREVIEW", 20, 15, width, height, 0.6f, 0.8f, 1.0f);
            draw(vertices);
        }
        return;
    }
    const char* modes[] = {"PATH TRACING", "PBR RAY TRACING", "LAMBERTIAN"};
    const auto number = [](float value, int precision) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(precision) << value;
        return out.str();
    };
    std::vector<std::string> lines;
    if (materialPage_ && !scene.materials.empty()) {
        const size_t index = std::min(materialIndex_, scene.materials.size() - 1);
        const Material& material = scene.materials[index];
        lines = {
            "MATERIAL: " + std::to_string(index),
            "ALBEDO R: " + number(material.albedo.r, 2),
            "ALBEDO G: " + number(material.albedo.g, 2),
            "ALBEDO B: " + number(material.albedo.b, 2),
            "METALLIC: " + number(material.metallic, 2),
            "ROUGHNESS: " + number(material.roughness, 2),
            "REFLECTIVITY: " + number(material.reflectivity, 2),
            "TRANSPARENCY: " + number(material.transparency, 2),
            "IOR: " + number(material.ior, 2),
            "SSS DISTANCE: " + number(material.subsurfaceMeanFreePath, 2),
            "BASE MAP: " + (material.baseColorTexture < 0 ? std::string("NONE") :
                            material.baseColorTexture == 0 ? std::string("CHECKER") :
                            "PNG " + std::to_string(material.baseColorTexture))
        };
    } else if (materialPage_) {
        lines = {"NO MATERIALS"};
    } else {
        lines = {
            std::string("MODE: ") + modes[static_cast<int>(config.shadingMode)],
            "BOUNCES: " + std::to_string(config.pathTracerBounceBudget),
            std::string("ACCUMULATION: ") + (config.accumulationEnabled ? "ON" : "OFF"),
            "EXPOSURE: " + number(config.displayExposure, 2),
            "TRACE AMBIENT: " + number(config.pathTracerAmbient, 2),
            "EDITOR AMBIENT: " + number(config.rasterAmbient, 2),
            "POINT POWER: " + number(config.pointLightPower, 1),
            "SUN POWER: " + number(config.directionalLightPower, 2),
            std::string("SSS MASTER: ") + (config.subsurfaceEnabled ? "ON" : "OFF"),
            std::string("CAUSTICS: ") + (config.causticsEnabled ? "ON" : "OFF"),
            "PHOTONS: " + std::to_string(config.causticPhotonCount)
        };
    }
    const int selected = materialPage_ ? materialSelected_ : selected_;
    std::vector<Vertex> vertices;
    const float footerY = 63.0f + 22.0f * static_cast<float>(lines.size()) + 8.0f;
    rectangle(vertices, 8, 8, 350, footerY + 18.0f, width, height, 0.04f, 0.06f, 0.10f);
    text(vertices, editorPreview ? "EDITOR PREVIEW" : materialPage_ ? "MATERIAL SETTINGS" : "RENDER SETTINGS",
         20, 18, width, height, 0.6f, 0.8f, 1.0f);
    text(vertices, "FPS: " + number(fps, 1), 20, 40, width, height, 0.6f, 0.8f, 1.0f);
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (selected == i) rectangle(vertices, 14, 61 + 22 * i, 338, 19, width, height, 0.15f, 0.25f, 0.38f);
        text(vertices, (selected == i ? "> " : "  ") + lines[i], 20, 63 + 22 * i,
             width, height, 0.95f, 0.95f, 0.95f);
    }
    text(vertices, materialPage_ ? "TAB PAGE F4 SAVE F3 CLOSE" : "TAB PAGE  F3 CLOSE",
         20, footerY, width, height, 0.6f, 0.8f, 1.0f);

    draw(vertices);
}

} // namespace rayzen
