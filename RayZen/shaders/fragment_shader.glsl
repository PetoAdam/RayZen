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

struct Light {
    vec4 positionOrDirection;  // Position for point lights, Direction for directional lights
    vec3 color;     // Light color
};

uniform Camera camera;
uniform Sphere spheres[10];
uniform Material materials[10];
uniform Light lights[10];

out vec4 FragColor;

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

bool hitSphere(Sphere sphere, Ray ray, out vec3 hitPoint, out vec3 normal, out int materialIndex) {
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

vec3 calculateLighting(vec3 hitPoint, vec3 normal, Material material, vec3 viewDir) {
    vec3 finalColor = ambientLightColor * material.diffuse; // Ambient lighting

    // Loop over all lights
    for (int i = 0; i < 10; ++i) {
        Light light = lights[i];

        vec3 lightDir;
        if (light.positionOrDirection.w == 1.0) {
            // Point light
            lightDir = normalize(light.positionOrDirection.xyz - hitPoint);
        } else {
            // Directional light
            lightDir = normalize(light.positionOrDirection.xyz);
        }

        // Diffuse
        float diff = max(dot(normal, lightDir), 0.0);
        vec3 diffuse = diff * material.diffuse * light.color;

        // Specular
        vec3 reflectDir = reflect(-lightDir, normal);
        float spec = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);
        vec3 specular = spec * material.specular * light.color;

        finalColor += diffuse + specular;
    }

    return finalColor;
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(800.0, 600.0); // Screen size hardcoded
    Ray ray = calculateRay(uv);

    vec3 finalColor = vec3(0.0); // Default color
    for (int i = 0; i < 10; ++i) { // Loop over all spheres
        vec3 hitPoint, normal;
        int materialIndex;
        if (hitSphere(spheres[i], ray, hitPoint, normal, materialIndex)) {
            vec3 hitPoint, normal;
            int materialIndex;
            if (hitSphere(spheres[i], ray, hitPoint, normal, materialIndex)) {
                vec3 viewDir = normalize(camera.position - hitPoint);
                finalColor = calculateLighting(hitPoint, normal, materials[materialIndex], viewDir);
            }
        }
    }
    FragColor = vec4(finalColor, 1.0);
}
