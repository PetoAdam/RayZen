#ifndef RAYZEN_RENDERER_SETTINGS_PANEL_H
#define RAYZEN_RENDERER_SETTINGS_PANEL_H

#include "Renderer/GLResources.h"
#include "Renderer/RenderConfig.h"
#include "Scene.h"

struct GLFWwindow;

namespace rayzen {

class SettingsPanel {
public:
    ~SettingsPanel();
    bool initialize(const std::string& vertexPath, const std::string& fragmentPath);
    bool update(GLFWwindow* window, RenderConfig& config, Scene& scene);
    void render(int width, int height, const RenderConfig& config, const Scene& scene, float fps, bool editorPreview);
    bool consumeSaveRequested() noexcept;

private:
    GLProgram program_;
    GLuint vao_ = 0;
    GLuint vbo_ = 0;
    bool visible_ = false;
    int selected_ = 0;
    int materialSelected_ = 0;
    size_t materialIndex_ = 0;
    bool materialPage_ = false;
    bool saveRequested_ = false;
    bool keys_[7]{};
};

} // namespace rayzen

#endif
