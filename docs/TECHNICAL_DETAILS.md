# RayZen: Technical and Mathematical Documentation

## Table of Contents
1. Introduction
2. System Architecture
3. Path Tracing Algorithm
4. BVH Construction and Traversal
5. Ray-Primitive Intersection
6. Physically-Based Materials
7. Lighting Model
8. GPU Data Flow, SSBOs, and Caching
9. Debug Features and Dynamic Scenes
10. Performance Considerations
11. References

---

## 1. Introduction
RayZen is a real-time path tracer leveraging OpenGL compute and fragment shaders for physically-based rendering. This document provides a deep technical and mathematical overview of the system, suitable for advanced users, researchers, and students.

---

## 2. System Architecture
- **C++ Core**: Scene setup, mesh loading, BVH construction, dynamic scene management, and OpenGL resource management.
- **GLSL Shaders**: Path tracing, BVH traversal, and physically-based shading.
- **Data Flow**: Scene data is uploaded to the GPU via Shader Storage Buffer Objects (SSBOs). BVH and triangle data are cached to disk for fast startup.

---

## 3. Path Tracing Algorithm
RayZen implements unidirectional path tracing:
- For each pixel, a primary ray is generated from the camera.
- Rays are traced through the scene, bouncing off surfaces according to material properties.
- At each intersection, direct and indirect lighting is computed.
- Russian roulette is used for path termination.

**Mathematical Formulation:**
The rendering equation:

$$
L_o(x, \omega_o) = L_e(x, \omega_o) + \int_{\Omega} f_r(x, \omega_i, \omega_o) L_i(x, \omega_i) (\omega_i \cdot n) d\omega_i
$$

Where:
- $L_o$ is outgoing radiance
- $L_e$ is emitted radiance
- $f_r$ is the BRDF
- $L_i$ is incoming radiance
- $n$ is the surface normal

---

## 4. BVH Construction and Traversal
- **BVH (Bounding Volume Hierarchy)**: A binary tree where each node contains an AABB (Axis-Aligned Bounding Box) enclosing a subset of triangles.
- **BLAS/TLAS**: Bottom-level BVHs (BLAS) are built per mesh; a top-level BVH (TLAS) is built over mesh instances for instancing and dynamic scenes.
- **Construction**: Surface Area Heuristic (SAH) or midpoint splitting is used to partition triangles. BVH and triangle data are cached to disk for fast startup.
- **Dynamic Scenes**: BVH and SSBOs are rebuilt every frame for moving objects.
- **Traversal**: On the GPU, a stack-based traversal is implemented in GLSL. Only triangles in leaf nodes are tested for intersection.

**AABB Intersection:**
For a ray $r(t) = o + td$ and box $[b_{min}, b_{max}]$:

$$
t_{min} = \max(\min((b_{min} - o) / d, (b_{max} - o) / d))
$$
$$
t_{max} = \min(\max((b_{min} - o) / d, (b_{max} - o) / d))
$$

If $t_{max} \geq \max(t_{min}, 0)$, the ray intersects the box.

---

## 5. Ray-Primitive Intersection
- **Triangles**: Möller–Trumbore algorithm is used for efficient ray-triangle intersection.

**Algorithm:**
Given triangle vertices $v_0$, $v_1$, $v_2$ and ray $(o, d)$:
1. $e_1 = v_1 - v_0$, $e_2 = v_2 - v_0$
2. $h = d \times e_2$, $a = e_1 \cdot h$
3. If $|a| < \epsilon$, no intersection.
4. $f = 1/a$, $s = o - v_0$
5. $u = f (s \cdot h)$, $v = f (d \cdot (s \times e_1))$
6. If $u < 0$ or $u > 1$ or $v < 0$ or $u + v > 1$, no intersection.
7. $t = f (e_2 \cdot (s \times e_1))$
8. If $t > \epsilon$, intersection at $o + td$.

---

## 6. Physically-Based Materials
- **Material Parameters**: Albedo, metallic, roughness, reflectivity, transparency, index of refraction (IOR).
- **BRDF**: Microfacet GGX for specular, Lambertian for diffuse.
- **Fresnel**: Schlick's approximation.

---

## 7. Lighting Model
- **Point and Directional Lights**: Both supported, with physically-based attenuation.
- **Multiple Importance Sampling**: Not yet implemented, but the code is structured for future extension.
- **Shadow Rays**: Use BVH for occlusion checks.

---

## 8. GPU Data Flow, SSBOs, and Caching
- **Triangles, Materials, Lights, BVH Nodes, and Indices** are uploaded to the GPU as SSBOs.
- **Shader Bindings**:
    - 0: Triangles
    - 1: Materials
    - 2: Lights
    - 5: TLAS Nodes
    - 6: TLAS Triangle Indices
    - 7: BLAS Nodes
    - 8: BLAS Triangle Indices
    - 9: BVH Instances
- **BVH/SSBO Caching**: BVH and triangle data are cached to `build/bvh_cache/` for fast startup. If geometry or transforms change, the cache is rebuilt.
- **Camera and other uniforms** are sent per-frame.

---

## 9. Debug Features and Dynamic Scenes
- **Debug Overlays**: Toggle light markers, BVH wireframes, and BLAS/TLAS debug modes with keyboard shortcuts (L, B, N).
- **Dynamic Scene Support**: BVH and SSBOs are rebuilt every frame for moving objects and animated meshes.
- **Performance Logging**: Shader compile times, buffer upload times, and FPS are logged to the terminal.

---

## 10. Performance Considerations
- **BVH**: Reduces intersection tests from $O(N)$ to $O(\log N)$ per ray.
- **GPU Parallelism**: Each pixel is computed independently in the fragment shader.
- **Dynamic Scenes**: For moving meshes, the BVH must be rebuilt and re-uploaded each frame.
- **SSBO Caching**: Reduces startup time by reusing previous BVH/triangle data if geometry is unchanged.
- **Russian Roulette**: Used to probabilistically terminate low-contribution paths.

---

## 11. References
- [Physically Based Rendering: From Theory to Implementation](https://www.pbr-book.org/)
- [Real-Time Rendering, 4th Edition](https://www.realtimerendering.com/)
- [Möller–Trumbore Intersection Algorithm](https://en.wikipedia.org/wiki/Möller–Trumbore_intersection_algorithm)
- [OpenGL 4.3 Specification](https://www.khronos.org/registry/OpenGL/specs/gl/glspec43.core.pdf)

---

For further questions or contributions, please see the main [README.md](../README.md) or open an issue on GitHub.
