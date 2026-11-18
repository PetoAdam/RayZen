cmake_minimum_required(VERSION 3.10)
project(RayZen)

# Set the OpenGL policy to prefer GLVND
cmake_policy(SET CMP0072 NEW)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(glm REQUIRED)
find_package(GLEW REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(assimp CONFIG REQUIRED)
find_package(PNG REQUIRED)

# Add the source files explicitly so generated build rules do not keep stale references to removed demo files.
set(RAYZEN_SOURCES
	${CMAKE_SOURCE_DIR}/src/main.cpp
	${CMAKE_SOURCE_DIR}/src/Mesh.cpp
	${CMAKE_SOURCE_DIR}/src/Texture.cpp
	${CMAKE_SOURCE_DIR}/src/BVH.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/RenderConfig.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/MaterialSettings.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/SettingsPanel.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/WarmWindow.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/GLResources.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/SceneGPU.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/CausticPass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/InputController.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/RasterPass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/PathTracerPass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/DisplayResolvePass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/LegacyFragmentPass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/Renderer.cpp
)

# Create the executable from the source files
add_executable(RayZen ${RAYZEN_SOURCES})
target_compile_definitions(RayZen PRIVATE RAYZEN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_include_directories(RayZen PRIVATE ${CMAKE_SOURCE_DIR}/include)
find_package(Threads REQUIRED)

# Manually link GLFW, OpenGL, GLEW, and Assimp
target_link_libraries(RayZen glfw OpenGL::GL GLEW::GLEW glm::glm assimp::assimp Threads::Threads PNG::PNG)

enable_testing()

add_executable(rayzen_smoke_tests
	${CMAKE_SOURCE_DIR}/tests/core_smoke.cpp
	${CMAKE_SOURCE_DIR}/src/Mesh.cpp
	${CMAKE_SOURCE_DIR}/src/BVH.cpp
)

target_include_directories(rayzen_smoke_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(rayzen_smoke_tests glm::glm)
target_compile_definitions(rayzen_smoke_tests PRIVATE RAYZEN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

add_test(NAME rayzen_smoke_tests COMMAND rayzen_smoke_tests)

add_executable(rayzen_cache_tests
	${CMAKE_SOURCE_DIR}/tests/cache_integration.cpp
	${CMAKE_SOURCE_DIR}/src/Texture.cpp
	${CMAKE_SOURCE_DIR}/src/BVH.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/GLResources.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/SceneGPU.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/CausticPass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/RenderConfig.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/MaterialSettings.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/PathTracerPass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/LegacyFragmentPass.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/RasterPass.cpp
)
target_include_directories(rayzen_cache_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(rayzen_cache_tests glfw OpenGL::GL GLEW::GLEW Threads::Threads glm::glm PNG::PNG)
target_compile_definitions(rayzen_cache_tests PRIVATE RAYZEN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME rayzen_cache_tests COMMAND rayzen_cache_tests)
set_tests_properties(rayzen_cache_tests PROPERTIES SKIP_RETURN_CODE 77)

add_executable(rayzen_settings_tests
	${CMAKE_SOURCE_DIR}/tests/settings_panel.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/SettingsPanel.cpp
	${CMAKE_SOURCE_DIR}/src/Renderer/GLResources.cpp
)
target_include_directories(rayzen_settings_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(rayzen_settings_tests glfw OpenGL::GL GLEW::GLEW glm::glm Threads::Threads)
add_test(NAME rayzen_settings_tests COMMAND rayzen_settings_tests)

set(STARTUP_TEST_PROJECT "${CMAKE_CURRENT_BINARY_DIR}/startup_test_project")
file(MAKE_DIRECTORY "${STARTUP_TEST_PROJECT}/shaders")
foreach(shader editor_vertex.glsl editor_fragment.glsl settings_panel.vert settings_panel.frag
               path_trace.comp realtime_ray.comp lambertian.comp path_trace_shared.glsl caustics.comp direct_lighting.glsl material_sampling.glsl pbr.glsl ray_tracing.glsl display_resolve.vert display_resolve.frag)
	configure_file("${CMAKE_SOURCE_DIR}/shaders/${shader}" "${STARTUP_TEST_PROJECT}/shaders/${shader}" COPYONLY)
endforeach()
set(STARTUP_TEST_SOURCES ${RAYZEN_SOURCES})
list(REMOVE_ITEM STARTUP_TEST_SOURCES "${CMAKE_SOURCE_DIR}/src/main.cpp")
add_executable(rayzen_startup_tests ${CMAKE_SOURCE_DIR}/tests/renderer_startup.cpp ${STARTUP_TEST_SOURCES})
target_compile_definitions(rayzen_startup_tests PRIVATE RAYZEN_SOURCE_DIR="${STARTUP_TEST_PROJECT}")
target_include_directories(rayzen_startup_tests PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(rayzen_startup_tests glfw OpenGL::GL GLEW::GLEW glm::glm assimp::assimp Threads::Threads PNG::PNG)
add_test(NAME rayzen_startup_tests COMMAND rayzen_startup_tests)
set_tests_properties(rayzen_startup_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
