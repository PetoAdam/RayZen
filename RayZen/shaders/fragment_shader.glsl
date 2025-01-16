#version 330 core

uniform vec2 u_resolution;       // Resolution of the window
uniform vec3 u_sphere1;          // Sphere 1 (x, y, radius)
uniform vec3 u_sphere2;          // Sphere 2 (x, y, radius)

out vec4 color;

// Struct to define a ray
struct Ray {
    vec3 origin;
    vec3 direction;
};

// Function to compute the intersection of a ray with a sphere
bool intersectSphere(Ray ray, vec3 sphereCenter, float sphereRadius, out float t) {
    vec3 oc = ray.origin - sphereCenter;
    float a = dot(ray.direction, ray.direction);
    float b = 2.0 * dot(oc, ray.direction);
    float c = dot(oc, oc) - sphereRadius * sphereRadius;
    float discriminant = b * b - 4.0 * a * c;

    if (discriminant < 0.0) {
        t = -1.0;
        return false;
    } else {
        t = (-b - sqrt(discriminant)) / (2.0 * a);
        if (t < 0.0) {
            t = (-b + sqrt(discriminant)) / (2.0 * a);
        }
        return t >= 0.0;
    }
}

// Function to compute shading with ambient, diffuse, and specular components
vec3 computeShading(vec3 hitPoint, vec3 normal, vec3 lightDir, vec3 sphereColor, vec3 cameraOrigin) {
    vec3 ambient = 0.1 * sphereColor; // Ambient component

    // Diffuse component
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = diff * sphereColor;

    // Specular component
    vec3 viewDir = normalize(cameraOrigin - hitPoint);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 16.0); // Shininess factor
    vec3 specular = vec3(1.0) * spec; // White specular highlights

    return ambient + diffuse + specular;
}

void main() {
    vec2 uv = gl_FragCoord.xy / u_resolution;
    uv = uv * 2.0 - 1.0; // Map to [-1, 1]

    // Correct for aspect ratio
    float aspectRatio = u_resolution.x / u_resolution.y;
    uv.x *= aspectRatio;

    vec3 cameraOrigin = vec3(0.0, 0.0, -1.0); // Camera position
    vec3 rayDir = normalize(vec3(uv, 1.0));   // Ray direction

    Ray ray = Ray(cameraOrigin, rayDir);

    // Sphere 1 intersection
    float t1;
    bool hit1 = intersectSphere(ray, vec3(u_sphere1.xy, 0.0), u_sphere1.z, t1);

    // Sphere 2 intersection
    float t2;
    bool hit2 = intersectSphere(ray, vec3(u_sphere2.xy, 0.0), u_sphere2.z, t2);

    vec3 lightDir = normalize(vec3(1.0, 1.0, -1.0)); // Light direction
    vec3 backgroundColor = vec3(0.05, 0.05, 0.1);    // Background color

    if (hit1 || hit2) {
        float t = (t1 > 0.0 && t2 > 0.0) ? min(t1, t2) : max(t1, t2);
        vec3 hitPoint = ray.origin + t * ray.direction;

        vec3 sphereColor;
        vec3 sphereCenter;

        if (t == t1) {
            sphereColor = vec3(1.0, 0.0, 0.0); // Red for Sphere 1
            sphereCenter = vec3(u_sphere1.xy, 0.0);
        } else {
            sphereColor = vec3(0.0, 1.0, 0.0); // Green for Sphere 2
            sphereCenter = vec3(u_sphere2.xy, 0.0);
        }

        vec3 normal = normalize(hitPoint - sphereCenter);
        vec3 shadedColor = computeShading(hitPoint, normal, lightDir, sphereColor, cameraOrigin);
        color = vec4(shadedColor, 1.0);
    } else {
        color = vec4(backgroundColor, 1.0);
    }
}
