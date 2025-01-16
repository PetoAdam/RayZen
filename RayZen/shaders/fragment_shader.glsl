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
    float reflectivity;
    float transparency;
    float ior;  // Index of Refraction
};

struct Light {
    vec4 positionOrDirection;
    vec3 color;
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

vec3 calculateLighting(vec3 hitPoint, vec3 normal, Material material, vec3 viewDir) {
    vec3 finalColor = ambientLightColor * material.diffuse;

    for (int i = 0; i < 10; ++i) {
        Light light = lights[i];

        vec3 lightDir = normalize(light.positionOrDirection.xyz);
        float diff = max(dot(normal, lightDir), 0.0);
        vec3 diffuse = diff * material.diffuse * light.color;

        vec3 reflectDir = reflect(-lightDir, normal);
        float spec = pow(max(dot(viewDir, reflectDir), 0.0), material.shininess);
        vec3 specular = spec * material.specular * light.color;

        finalColor += diffuse + specular;
    }

    return finalColor;
}

vec3 reflectRay(vec3 incident, vec3 normal) {
    return incident - 2.0 * dot(incident, normal) * normal;
}

vec3 refractRay(vec3 incident, vec3 normal, float ior) {
    float cosi = clamp(dot(incident, normal), -1.0, 1.0);
    float etai = 1.0, etat = ior;
    vec3 n = normal;
    if (cosi < 0.0) { cosi = -cosi; } else { 
        etat = etai;
        etai = ior;
        n = -normal;
    }
    float eta = etai / etat;
    float k = 1.0 - eta * eta * (1.0 - cosi * cosi);
    return (k < 0.0) ? vec3(0.0) : eta * incident + (eta * cosi - sqrt(k)) * n;
}

void main() {
    vec2 uv = gl_FragCoord.xy / vec2(800.0, 600.0);
    Ray ray = calculateRay(uv);

    // Path tracing iteration
    vec3 color = vec3(0.0);
    vec3 accumulatedColor = vec3(0.0);
    vec3 currentRayOrigin = ray.origin;
    vec3 currentRayDirection = ray.direction;

    int maxBounces = 5;
    float reflectivityFactor, transparencyFactor;

    for (int bounce = 0; bounce < maxBounces; ++bounce) {
        vec3 hitPoint, normal;
        int materialIndex = -1;
        float closestT = 1.0e30;
        Material closestMaterial;

        // Find closest intersection
        for (int i = 0; i < 10; ++i) {
            vec3 tempHitPoint, tempNormal;
            int tempMaterialIndex = -1;
            if (hitSphere(spheres[i], Ray(currentRayOrigin, currentRayDirection), tempHitPoint, tempNormal, tempMaterialIndex)) {
                float t = length(tempHitPoint - currentRayOrigin);
                if (t < closestT) {
                    closestT = t;
                    hitPoint = tempHitPoint;
                    normal = tempNormal;
                    materialIndex = tempMaterialIndex;
                    closestMaterial = materials[materialIndex];
                }
            }
        }

        if (materialIndex == -1) break;  // No intersection, exit loop

        // Calculate lighting and color for the closest intersection
        vec3 viewDir = normalize(camera.position - hitPoint);
        accumulatedColor = calculateLighting(hitPoint, normal, closestMaterial, viewDir);

        // Reflection
        reflectivityFactor = closestMaterial.reflectivity;
        if (reflectivityFactor > 0.0) {
            currentRayOrigin = hitPoint;
            currentRayDirection = reflectRay(currentRayDirection, normal);
            accumulatedColor = mix(accumulatedColor, accumulatedColor, reflectivityFactor);
        }

        // Refraction
        transparencyFactor = closestMaterial.transparency;
        if (transparencyFactor > 0.0) {
            currentRayOrigin = hitPoint;
            currentRayDirection = refractRay(currentRayDirection, normal, closestMaterial.ior);
            accumulatedColor = mix(accumulatedColor, accumulatedColor, transparencyFactor);
        }

        // Accumulate color
        color = accumulatedColor;
    }

    FragColor = vec4(color, 1.0);
}
