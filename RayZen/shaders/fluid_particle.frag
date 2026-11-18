#version 430 core
in float speed;
out vec4 color;

void main() {
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    float r2 = dot(p, p);
    if (r2 >= 1.0) discard;
    vec3 slow = vec3(0.06, 0.38, 0.85);
    vec3 fast = vec3(0.32, 0.95, 0.97);
    vec3 base = mix(slow, fast, clamp(speed * 0.35, 0.0, 1.0));
    float light = 0.55 + 0.45 * sqrt(1.0 - r2);
    color = vec4(base * light, 1.0 - smoothstep(0.78, 1.0, r2));
}
