#version 430 core
struct Particle { vec4 position; vec4 velocity; };
layout(std430, binding = 0) readonly buffer Positions { Particle particles[]; };
uniform mat4 view;
uniform mat4 projection;
uniform float viewportHeight;
uniform float radius;
out float eyeDepth;
out float speed;

void main() {
    Particle particle = particles[gl_VertexID];
    vec4 eye = view * particle.position;
    gl_Position = projection * eye;
    eyeDepth = -eye.z;
    speed = length(particle.velocity.xyz);
    gl_PointSize = max(1.0, viewportHeight * projection[1][1] * radius / max(eyeDepth, 0.01));
}
