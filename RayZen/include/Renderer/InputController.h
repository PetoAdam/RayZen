#ifndef RAYZEN_RENDERER_INPUT_CONTROLLER_H
#define RAYZEN_RENDERER_INPUT_CONTROLLER_H

struct GLFWwindow;

#include "Camera.h"
#include "Scene.h"
#include "Renderer/DebugState.h"
#include "Renderer/RenderConfig.h"

namespace rayzen {

// Centralizes keyboard/mouse handling. Drives camera updates and toggles
// DebugState flags. Returns hints to the caller via FrameInputResult so the
// orchestrator can reset accumulation or flip the swap interval.
struct FrameInputResult {
    bool requestEditorToggle = false;
    bool requestSwapToggle = false;
    bool cameraMoved = false; // true if the camera state changed this frame
};

class InputController {
public:
    InputController() = default;

    FrameInputResult update(GLFWwindow* window,
                            float deltaTime,
                            int viewportW,
                            int viewportH,
                            Camera& camera,
                            DebugState& debug,
                            const Scene& scene);

private:
    bool editorKeyPressed_ = false;
    bool swapKeyPressed_ = false;
    bool lKeyPressed_ = false;
    bool bKeyPressed_ = false;
    bool nKeyPressed_ = false;
    bool dragging_ = false;
    double lastMouseX_ = 0.0;
    double lastMouseY_ = 0.0;
};

} // namespace rayzen

#endif
