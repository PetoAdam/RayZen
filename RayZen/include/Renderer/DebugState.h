#ifndef RAYZEN_RENDERER_DEBUG_STATE_H
#define RAYZEN_RENDERER_DEBUG_STATE_H

namespace rayzen {

// Debug toggles bound to keyboard/mouse interaction. Keep this a plain data struct
// so passes can read it without owning input policy.
struct DebugState {
    bool showLights = false;
    bool showBVH = false;
    int  bvhMode = 0;          // 0 = TLAS, 1 = BLAS-branch
    int  selectedBLAS = 0;
    int  selectedTri = 0;
    bool editorMode = false;   // raster fallback
    bool userLockedEditorMode = false;
};

} // namespace rayzen

#endif
