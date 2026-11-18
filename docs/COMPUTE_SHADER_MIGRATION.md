# Compute Shader Migration Plan

## Current State

RayZen currently path traces in `shaders/fragment_shader.glsl` by drawing a full-screen quad. That works, but it couples sample generation, accumulation, display mapping, and presentation to a raster draw call. The current bottlenecks are mostly shader-side traversal and divergence, but there is also avoidable GL driver overhead from per-frame uniform lookups and redundant binding work on the CPU side.

The repo already has the right building blocks for a compute migration:

- Scene data lives in SSBOs.
- Accumulation already uses an `rgba32f` image.
- TLAS and BLAS traversal are already shader-driven.
- The renderer already distinguishes between interactive raster mode and path tracer mode.

## Why Move To Compute

The main benefits for this codebase are:

- Decouple path tracing from raster presentation.
- Dispatch work in tiles or fixed workgroups instead of one fragment invocation per pixel.
- Add more explicit scheduling for accumulation, denoise, reprojection, and debug passes.
- Make future optimizations like wave-friendly traversal, adaptive sampling, and compaction easier to stage.

Compute will not automatically fix divergence in TLAS/BLAS traversal, but it removes the full-screen draw dependency and gives more control over memory access and multi-pass structure.

## Recommended Target Layout

Use a small pass graph instead of one monolithic compute shader.

1. `path_trace.comp`
   Reads camera + scene SSBOs, traces one sample per pixel, writes radiance into an accumulation image.

2. `display_resolve.comp` or fragment resolve
   Reads accumulation, applies exposure and tone mapping, writes a display texture.

3. Optional later passes
   `reprojection.comp`, `variance.comp`, `denoise.comp`, `debug_overlay.comp`.

Keep the existing raster editor path unchanged during the first migration stage.

## Phase Plan

### Phase 1: Stabilize Interfaces

- Keep the existing SSBO layouts exactly as they are.
- Move renderer settings into a shared CPU-side config object.
- Treat camera state, accumulation state, and render settings as explicit inputs to the path tracing pass.

This is mostly done by the current runtime-config work and uniform cleanup.

### Phase 2: Extract Path Tracing Core

- Copy the path tracing logic from `fragment_shader.glsl` into a new `path_trace.comp`.
- Preserve TLAS/BLAS traversal code unchanged at first.
- Keep the same accumulation format and frame-index seeding so image comparisons stay meaningful.
- Dispatch with `local_size_x = 8`, `local_size_y = 8` first. That is a safe starting point on Mesa and easy to profile against `16x16`.

Validation target:

- Same camera and config should produce visually matching output versus the fragment path tracer.
- Noise pattern can differ, but mean brightness and material response should match.

### Phase 3: Separate Resolve From Sampling

- Remove tone mapping from the tracing pass.
- Write linear HDR radiance only in the compute pass.
- Add a resolve pass that applies exposure, ACES, gamma, and overlays.

This matters because it lets the renderer skip retracing when only display settings change.

### Phase 4: Add Persistent Images And Ping-Pong Buffers

Introduce explicit textures for:

- `accumulation_hdr`
- `display_ldr`
- optional `history_hdr`
- optional `moments_or_variance`

Use barriers narrowly:

- `GL_SHADER_IMAGE_ACCESS_BARRIER_BIT` after path tracing
- `GL_TEXTURE_FETCH_BARRIER_BIT` only where resolve or debug passes sample fresh writes

### Phase 5: Improve GPU Occupancy

Once compute is correct, tackle the real performance work:

- Sort or bucket rays by bounce depth only if profiling justifies it.
- Consider splitting shadow rays from indirect rays.
- Move toward iterative queues only after the simple per-pixel compute path is stable.
- Profile `8x8` vs `16x16` workgroups on the target Mesa driver.

Do not start with wavefront/path compaction. The current repo needs a correct and measurable baseline first.

## CPU-Side Renderer Changes

Add a thin renderer abstraction rather than growing `main.cpp` further.

Suggested split:

- `RenderConfig` for hot-reloadable settings
- `PathTracerResources` for textures, SSBO bindings, and shader handles
- `PathTracerDispatcher` for dispatch sizing and barriers
- `DisplayResolver` for tone mapping and final presentation

The first extraction should move path tracer setup and dispatch out of `main.cpp`, not change scene ownership.

## Data Compatibility Rules

To reduce migration risk:

- Keep current SSBO bindings for triangles, materials, lights, TLAS, BLAS, and instances.
- Keep the accumulation image format as `RGBA32F` during migration.
- Keep BVH cache serialization unchanged.
- Keep the raster editor mode independent from compute migration.

That lets the compute implementation reuse almost all current scene upload code.

## Immediate Performance Follow-Ups

After the compute baseline lands, the next likely wins are:

1. Separate resolve from tracing so exposure changes do not force new samples.
2. Avoid updating scene/light buffers when config values have not changed.
3. Add a low-resolution interactive mode controlled by config for camera movement.
4. Investigate a more GPU-friendly TLAS builder only after dispatch-side profiling.

## Validation Checklist

- Build both raster and compute paths at the same time.
- Keep the fragment path tracer behind a runtime toggle until compute matches it.
- Compare frame time, brightness, and shadow stability using the same camera and config.
- Verify accumulation reset rules on camera movement and config edits.
- Keep `rayzen_smoke_tests` green while refactoring CPU-side upload code.

## Recommended Order

1. Finish CPU-side cleanup and hot-reloadable settings.
2. Add compute path tracer with the current SSBO layouts.
3. Add a separate resolve pass.
4. Profile workgroup sizing and memory barriers.
5. Only then consider deeper traversal or queue-based redesigns.