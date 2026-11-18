#ifndef RAYZEN_RENDERER_CAUSTIC_PASS_H
#define RAYZEN_RENDERER_CAUSTIC_PASS_H

#include "Renderer/GLResources.h"
#include "Renderer/RenderConfig.h"
#include "Scene.h"

namespace rayzen {

class CausticPass {
public:
    ~CausticPass();
    bool initialize(const std::string& path);
    bool render(const Scene& scene, const RenderConfig& config, int width, int height, int frame);
    bool ready() const noexcept { return program_.valid(); }
    GLuint buffer() const noexcept { return pixels_; }

private:
    GLProgram program_;
    GLuint pixels_ = 0;
    GLuint texture_ = 0;
    int width_ = 0;
    int height_ = 0;
};

}
#endif
