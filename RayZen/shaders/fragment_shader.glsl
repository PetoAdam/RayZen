#version 330 core
in vec3 fragColor; // Color received from the vertex shader
out vec4 color;

void main() {
    color = vec4(fragColor, 1.0); // Set the color of the fragment
}
