#ifndef RAYZEN_RENDERER_RENDERER_H
#define RAYZEN_RENDERER_RENDERER_H

struct GLFWwindow;

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>

#include "Scene.h"
#include "Renderer/RenderConfig.h"
#include "Renderer/DebugState.h"
#include "Renderer/SceneGPU.h"
#include "Renderer/CausticPass.h"
#include "Renderer/PathTracerPass.h"
#include "Renderer/DisplayResolvePass.h"
#include "Renderer/LegacyFragmentPass.h"
#include "Renderer/RasterPass.h"
#include "Renderer/InputController.h"
#include "Renderer/SettingsPanel.h"
#include "Renderer/WarmWindow.h"

namespace rayzen {

struct RendererOptions {
    std::filesystem::path configPath;
    bool forceRebuildBVH = false;
    bool cacheOnly = false;
    int  warmupFrames = 0;
    int  benchmarkFrames = 0;
    bool requestPathTracerOnly = false;
    bool startInEditorMode = false;
    bool preferFragmentToggleFromEditor = false;
    bool disableAsyncFragmentCompile = false;
    bool asyncComputeJit = false;          // launch JIT prewarm in background thread
    bool swapIntervalOverridden = false;
    int  overrideSwapInterval = 0;
    bool requireComputeWhenConfigured = true;
    bool allowSoftwareRenderer = false;
    bool keepWarm = false;
};

// Top-level rendering orchestrator. Owns the GPU resources, every render pass,
// the runtime config watcher, and the input controller. main() constructs one,
// calls run() and the renderer drives the frame loop until the window closes.
class Renderer {
public:
    Renderer(GLFWwindow* window,
             Scene& scene,
             const std::filesystem::path& projectRoot,
             RendererOptions options);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool initialize();
    bool run();

    void onFramebufferResized(int width, int height);
    void hideWarmWindow();

private:
    // Frame loop helpers.
    void pickupAsyncPathTracer();
    bool renderFrame();
    void resetAccumulation();
    void applyConfig(const RenderConfig& next);
    bool runWarmup();
    bool maybeActivateComputePathTracer();
    bool launchAsyncComputeJit();
    void joinAsyncComputeJit();
    bool warmupRequested() const noexcept { return options_.warmupFrames > 0; }

    // Async path tracer compile: compiles fragment_shader.glsl in a hidden context
    // so the user can interact via raster mode while it links. Compute backend
    // does not need this (compute shader links quickly).
    bool launchAsyncFragmentCompile();
    void joinAsyncCompile();

    // Borrowed inputs.
    GLFWwindow* window_;
    Scene& scene_;
    std::filesystem::path projectRoot_;
    RendererOptions options_;

    // Owned subsystems.
    std::unique_ptr<RenderConfigWatcher> configWatcher_;
    SceneGPU sceneGPU_;
    CausticPass causticPass_;
    PathTracerPass pathTracer_;
    DisplayResolvePass resolvePass_;
    LegacyFragmentPass legacyPass_;
    RasterPass rasterPass_;
    InputController input_;
    SettingsPanel settingsPanel_;
    WarmWindow warmWindow_;
    bool warmWindowHidden_ = false;
    DebugState debug_;

    // Frame state.
    int viewportWidth_ = 0;
    int viewportHeight_ = 0;
    int frameCounter_ = 0;
    int accumulatedFrames_ = 0;
    bool accumulationDirty_ = true;
    int requestedSwapInterval_ = 0;
    glm::mat4 accumulationView_{1.0f};
    glm::mat4 accumulationProj_{1.0f};
    glm::vec3 accumulationCameraPos_{0.0f};
    float lastFrameTime_ = 0.0f;
    float deltaTime_ = 0.0f;
    float smoothedFrameTime_ = 0.0f;
    int totalTriangles_ = 0;

    // Async fragment shader compile.
    std::atomic<bool> fragmentReady_{false};
    std::atomic<GLuint> fragmentProgramHandle_{0};
    std::atomic<double> fragmentCompileMs_{0.0};
    std::thread fragmentCompileThread_;
    GLFWwindow* fragmentCompileWindow_ = nullptr;
    bool awaitingFragmentReady_ = false;
    bool useFragmentBackend_ = false; // true once we picked the fragment backend
    bool legacyAdopted_ = false;
    bool loggedComputeStartupBudgetHint_ = false;
    bool computeStartupBudgetComplete_ = false;
    bool pendingComputeActivation_ = false;
    bool computePrewarmed_ = false;
    bool pendingJitActivation_ = false;
    // Async compute JIT prewarm.
    std::atomic<bool> computeJitReady_{false};
    std::thread computeJitThread_;
    GLFWwindow* computeJitWindow_ = nullptr;
};

} // namespace rayzen

#endif
