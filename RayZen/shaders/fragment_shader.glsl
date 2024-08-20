#version 330 core

struct Camera {
    mat4 viewMatrix;
    mat4 projectionMatrix;
    vec3 position;
};

struct Sphere {
    vec3 center;
    float radius;
    int materialIndex;
};

struct Material {
    vec3 diffuse;
    vec3 specular;
    float shininess;
    float metallic;
    float fuzz;
    vec3 albedo;
};

out vec4 FragColor;

// Demo camera setup (adjust the values as needed)
const Camera camera = Camera(
    mat4(1.0), // Identity matrix for view
    mat4(
        vec4(1.0 / (800.0 / 600.0), 0.0, 0.0, 0.0),
        vec4(0.0, 1.0, 0.0, 0.0),
        vec4(0.0, 0.0, -2.0, 0.0),
        vec4(0.0, 0.0, 0.0, 1.0)
    ), // Perspective projection matrix
    vec3(0.0, 0.0, 3.0) // Camera position
);

// Demo spheres
const Sphere spheres[2] = Sphere[](
    Sphere(vec3(0.3, 0.0, -1.0), 0.3, 0),  // Sphere 0
    Sphere(vec3(0.0, 0.0, -1.8), 0.7, 1)   // Sphere 1
);

// Demo materials
const Material materials[2] = Material[](
    Material(vec3(0.8, 0.3, 0.3), vec3(0.9, 0.9, 0.9), 32.0, 0.1, 0.0, vec3(0.8, 0.3, 0.3)), // Material 0
    Material(vec3(0.3, 0.8, 0.3), vec3(0.9, 0.9, 0.9), 64.0, 0.5, 0.1, vec3(0.3, 0.8, 0.3))  // Material 1
);

// Demo lights
const vec3 lightPositions[1] = vec3[](
    vec3(5.0, 5.0, 5.0) // Light position
);
const vec3 lightColors[1] = vec3[](
    vec3(1.0, 1.0, 1.0) // Light color
);
// Ambient light
const vec3 ambientLightColor = vec3(0.2, 0.2, 0.2);

struct Ray {
    vec3 origin;
    vec3 direction;
};

Ray calculateRay(vec2 uv) {
    vec4 ray_clip = vec4(uv * 2.0 - 1.0, -1.0, 1.0);
    vec4 ray_eye = inverse(camera.projectionMatrix) * ray_clip;
    ray_eye = vec4(ray_eye.xy, -1.0, 0.0);
    vec3 ray_world = (inverse(camera.viewMatrix) * ray_eye).xyz;
    return Ray(camera.position, normalize(ray_world));
}

bool hitSphere(const Sphere sphere, const Ray ray, out vec3 hitPoint, out vec3 normal, out int materialIndex) {
    vec3 oc = ray.origin - sphere.center;
    float a = dot(ray.direction, ray.direction);
    float b = 2.0 * dot(oc, ray.direction);
    float c = dot(oc, oc) - sphere.radius * sphere.radius;
    float discriminant = b * b - 4.0 * a * c;

    if (discriminant > 0.0) {
        float t = (-b - sqrt(discriminant)) / (2.0 * a);
        if (t > 0.0) {
            hitPoint = ray.origin + t * ray.direction;
            normal = normalize(hitPoint - sphere.center);
            materialIndex = sphere.materialIndex;
            return true;
        }
    }
    return false;
}

bool isInShadow(const vec3 hitPoint, const vec3 lightPos, const int numSpheres) {
    vec3 lightDir = normalize(lightPos - hitPoint);
    Ray shadowRay = Ray(hitPoint + lightDir * 0.001, lightDir); // Offset to avoid self-shadowing

    for (int i = 0; i < numSpheres; i++) {
        vec3 shadowHitPoint, shadowNormal;
        int shadowMaterialIndex;
        if (hitSphere(spheres[i], shadowRay, shadowHitPoint, shadowNormal, shadowMaterialIndex)) {
            float distanceToLight = length(lightPos - hitPoint);
            float distanceToShadowHit = length(shadowHitPoint - hitPoint);
            if (distanceToShadowHit < distanceToLight) {
                return true; // Shadowed by another sphere
            }
        }
    }
    return false; // Not in shadow
}

vec3 calculateLighting(const vec3 hitPoint, const vec3 normal, const Material material, const vec3 viewDir, const int numLights) {
    vec3 finalColor = ambientLightColor * material.diffuse; // Ambient lighting

    // Loop over all lights
    for (int i = 0; i < numLights; i++) {
        vec3 lightPos = lightPositions[i];
        vec3 lightColor = lightColors[i];

        // Check for shadow
        if (isInShadow(hitPoint, lightPos, 2)) {
            continue; // Skip this light if in shadow
        }

        // Diffuse
        vec3 lightDir = normalize(lightPos - hitPoint);
        float diff = max(dot(normal, lightDir), 0.0);
        vec3 diffuse = diff * material.diffuse * lightColor;

        // Specular
        vec3 reflectDir = reflect(-lightDir, normal);
        float spec = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);
        vec3 specular = spec * material.specular * lightColor;

        finalColor += diffuse + specular;
    }

    return finalColor;
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(800, 600); // Assuming screen size, adjust as necessary
    Ray ray = calculateRay(uv);

    vec3 finalColor = vec3(0.0); // Default color

    for (int i = 0; i < 2; i++) { // Loop over all spheres
        vec3 hitPoint, normal;
        int materialIndex;
        if (hitSphere(spheres[i], ray, hitPoint, normal, materialIndex)) {
            vec3 viewDir = normalize(camera.position - hitPoint);
            finalColor = calculateLighting(hitPoint, normal, materials[materialIndex], viewDir, 1);
            break; // Uncomment this line if you want to stop after the first hit
        }
    }

    FragColor = vec4(finalColor, 1.0);
}
