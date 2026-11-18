#version 430 core
in vec2 uv;
out vec4 color;
uniform sampler3D fieldTexture;
uniform int dimensions;
uniform mat4 inverseViewProjection;
uniform vec3 cameraPosition;
uniform float obstacleAngle;
uniform vec3 gridMin;
uniform vec3 gridMax;

const float surfaceDensity = 0.72;
const float rayStep = 0.045;

bool boxIntersection(vec3 origin, vec3 direction, vec3 low, vec3 high,
                     out float enter, out float exit) {
    vec3 safeDirection = mix(vec3(0.000001), direction, greaterThan(abs(direction), vec3(0.000001)));
    vec3 a = (low - origin) / safeDirection;
    vec3 b = (high - origin) / safeDirection;
    vec3 nearPlane = min(a, b), farPlane = max(a, b);
    enter = max(max(nearPlane.x, nearPlane.y), nearPlane.z);
    exit = min(min(farPlane.x, farPlane.y), farPlane.z);
    return exit >= max(enter, 0.0);
}

bool obstacleHit(vec3 origin, vec3 direction, out float distance, out vec3 normal) {
    float c = cos(obstacleAngle), s = sin(obstacleAngle);
    mat3 rotation = mat3(c, 0, -s, 0, 1, 0, s, 0, c);
    vec3 localOrigin = transpose(rotation) * (origin - vec3(0.0, -1.02, 0.0));
    vec3 localDirection = transpose(rotation) * direction;
    vec3 halfSize = vec3(0.17, 0.62, 0.48);
    float enter, exit;
    if (!boxIntersection(localOrigin, localDirection, -halfSize, halfSize, enter, exit)) return false;
    distance = enter > 0.0 ? enter : exit;
    vec3 point = localOrigin + localDirection * distance;
    vec3 relative = abs(point / halfSize);
    vec3 localNormal = relative.x > relative.y && relative.x > relative.z ? vec3(sign(point.x), 0, 0) :
                       (relative.y > relative.z ? vec3(0, sign(point.y), 0) : vec3(0, 0, sign(point.z)));
    normal = rotation * localNormal;
    return true;
}

vec3 environment(vec3 origin, vec3 direction, out float distance) {
    vec3 sky = mix(vec3(0.62, 0.75, 0.83), vec3(0.15, 0.32, 0.50),
                   clamp(direction.y * 0.7 + 0.3, 0.0, 1.0));
    distance = 1e20;
    vec3 result = sky;
    if (direction.y < -0.001) {
        float t = (-1.82 - origin.y) / direction.y;
        if (t > 0.0) {
            distance = t;
            vec2 ground = (origin + direction * t).xz;
            float checker = mod(floor(ground.x) + floor(ground.y), 2.0);
            vec3 floorColor = mix(vec3(0.31, 0.36, 0.38), vec3(0.48, 0.53, 0.54), checker);
            result = mix(floorColor, sky, 1.0 - exp(-t * 0.035));
        }
    }
    float obstacleDistance;
    vec3 normal;
    if (obstacleHit(origin, direction, obstacleDistance, normal) && obstacleDistance < distance) {
        distance = obstacleDistance;
        float diffuse = 0.35 + 0.65 * max(dot(normal, normalize(vec3(-0.4, 1.0, 0.5))), 0.0);
        result = vec3(0.66, 0.22, 0.16) * diffuse;
    }
    return result;
}

float densityAt(vec3 position) {
    vec3 coordinates = (position - gridMin) / (gridMax - gridMin);
    if (any(lessThan(coordinates, vec3(0.0))) || any(greaterThan(coordinates, vec3(1.0)))) return 0.0;
    return textureLod(fieldTexture, coordinates, 0.0).r;
}

void main() {
    if (dimensions == 2) {
        color = vec4(mix(vec3(0.025, 0.045, 0.09), vec3(0.08, 0.14, 0.22), uv.y), 1.0);
        return;
    }
    vec4 farPoint = inverseViewProjection * vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec3 ray = normalize(farPoint.xyz / farPoint.w - cameraPosition);
    float environmentDistance;
    vec3 bg = environment(cameraPosition, ray, environmentDistance);
    float entry, exit;
    if (!boxIntersection(cameraPosition, ray, gridMin, gridMax, entry, exit)) {
        color = vec4(bg, 1.0); return;
    }
    float t = max(entry, 0.0);
    float first = -1.0;
    float thickness = 0.0;
    for (int i = 0; i < 160 && t <= min(exit, environmentDistance); ++i, t += rayStep) {
        float density = densityAt(cameraPosition + ray * t);
        if (density >= surfaceDensity) {
            if (first < 0.0) first = t;
            thickness += rayStep;
        }
    }
    if (first < 0.0) { color = vec4(bg, 1.0); return; }
    float low = max(first - rayStep, max(entry, 0.0)), high = first;
    for (int i = 0; i < 5; ++i) {
        float mid = (low + high) * 0.5;
        if (densityAt(cameraPosition + ray * mid) >= surfaceDensity) high = mid;
        else low = mid;
    }
    vec3 hit = cameraPosition + ray * high;
    vec3 h = (gridMax - gridMin) / vec3(textureSize(fieldTexture, 0));
    vec3 gradient = vec3(
        densityAt(hit + vec3(h.x, 0, 0)) - densityAt(hit - vec3(h.x, 0, 0)),
        densityAt(hit + vec3(0, h.y, 0)) - densityAt(hit - vec3(0, h.y, 0)),
        densityAt(hit + vec3(0, 0, h.z)) - densityAt(hit - vec3(0, 0, h.z)));
    vec3 normal = length(gradient) > 0.00001 ? -normalize(gradient) : -ray;
    if (dot(normal, ray) > 0.0) normal = -normal;
    float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(-ray, normal), 0.0), 5.0);
    vec3 refracted = refract(ray, normal, 1.0 / 1.333);
    float unusedDistance;
    vec3 transmitted = environment(hit + refracted * 0.02, refracted, unusedDistance);
    vec3 reflected = environment(hit + normal * 0.02, reflect(ray, normal), unusedDistance);
    vec3 absorption = exp(-vec3(0.75, 0.19, 0.08) * thickness);
    float highlight = pow(max(dot(reflect(ray, normal), normalize(vec3(-0.4, 1.0, 0.5))), 0.0), 80.0);
    vec3 liquid = mix(transmitted * absorption + vec3(0.01, 0.05, 0.065), reflected, fresnel) + highlight;
    color = vec4(liquid, 1.0);
}
