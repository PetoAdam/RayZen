cmake_minimum_required(VERSION 3.10)
project(RayZen)

# Set the OpenGL policy to prefer GLVND
cmake_policy(SET CMP0072 NEW)

set(CMAKE_CXX_STANDARD 17)

# Specify the include directory for headers
include_directories(${CMAKE_SOURCE_DIR}/include)

# Manually specify GLFW include directory and library path
include_directories(/usr/include)
link_directories(/usr/lib/x86_64-linux-gnu)

# GLM
find_package(glm REQUIRED)
include_directories(${GLM_INCLUDE_DIRS})

# GLEW
find_package(GLEW REQUIRED)
include_directories(${GLEW_INCLUDE_DIRS})

# OpenGL
find_package(OpenGL REQUIRED)
include_directories(${OPENGL_INCLUDE_DIR})

# Assimp
find_package(assimp REQUIRED)
include_directories(${ASSIMP_INCLUDE_DIRS})

# Add the source files in the src folder
file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)

# Create the executable from the source files
add_executable(RayZen ${SOURCES})

# Manually link GLFW, OpenGL, GLEW, and Assimp
target_link_libraries(RayZen glfw GL GLEW::GLEW assimp)
