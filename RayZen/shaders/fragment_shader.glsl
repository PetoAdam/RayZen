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
    vec3 albedo;
    float metallic;
    float roughness;
    float reflectivity;
    float transparency;
    float ior;
};

struct Light {
    vec4 positionOrDirection;
    vec3 color;
    float power;
};

uniform Camera camera;
uniform Sphere spheres[10];
uniform Material materials[10];
uniform Light lights[10];

out vec4 FragColor;

// Ambient light
const vec3 ambientLightColor = vec3(0.1, 0.1, 0.1);

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

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 calculateLighting(vec3 hitPoint, vec3 normal, Material material, vec3 viewDir) {
    vec3 F0 = mix(vec3(0.04), material.albedo, material.metallic);
    vec3 finalColor = ambientLightColor * material.albedo;

    for (int i = 0; i < 10; ++i) {
        Light light = lights[i];
        vec3 lightDir;
        float attenuation = 1.0;

        if (light.positionOrDirection.w == 1.0) { // Point light
            vec3 lightVec = light.positionOrDirection.xyz - hitPoint;
            float distance = max(length(lightVec), 0.001);
            lightDir = normalize(lightVec);
            attenuation = light.power / (distance * distance);
        } else { // Directional light
            lightDir = normalize(light.positionOrDirection.xyz);
            attenuation = light.power;
        }

        vec3 halfwayDir = normalize(lightDir + viewDir);
        float NdotL = max(dot(normal, lightDir), 0.0);
        float NdotV = max(dot(normal, viewDir), 0.0);
        float NdotH = max(dot(normal, halfwayDir), 0.0);

        // Fresnel-Schlick approximation
        vec3 F = fresnelSchlick(max(dot(halfwayDir, viewDir), 0.0), F0);

        // GGX Normal Distribution Function (D)
        float alpha = material.roughness * material.roughness;
        float alpha2 = alpha * alpha;
        float denom = (NdotH * NdotH * (alpha2 - 1.0) + 1.0);
        float D = alpha2 / (3.14159 * denom * denom);

        // Smith's Schlick GGX Geometry Function (G)
        float k = (material.roughness + 1.0) * (material.roughness + 1.0) / 8.0;
        float G = NdotV / (NdotV * (1.0 - k) + k);
        G *= NdotL / (NdotL * (1.0 - k) + k);

        // Specular term
        float denominator = max(4.0 * NdotV * NdotL, 0.0001);
        vec3 specular = (F * D * G) / denominator;

        // Lambertian Diffuse Term
        vec3 diffuse = (1.0 - F) * material.albedo * NdotL / 3.14159;

        // Apply light attenuation and add to final color
        finalColor += max(vec3(0.0), (diffuse + specular) * light.color * attenuation);
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

    vec3 color = vec3(0.0);
    vec3 currentRayOrigin = ray.origin;
    vec3 currentRayDirection = ray.direction;

    int maxBounces = 5;
    for (int bounce = 0; bounce < maxBounces; ++bounce) {
        vec3 hitPoint, normal;
        int materialIndex = -1;
        float closestT = 1.0e30;
        Material closestMaterial;

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

        if (materialIndex == -1) break;

        vec3 viewDir = normalize(camera.position - hitPoint);
        color += calculateLighting(hitPoint, normal, closestMaterial, viewDir);

        // Reflection and refraction
        if (closestMaterial.reflectivity > 0.0) {
            currentRayDirection = reflectRay(currentRayDirection, normal);
            currentRayOrigin = hitPoint + currentRayDirection * 0.001; // Offset to avoid self-intersection
        } else if (closestMaterial.transparency > 0.0) {
            currentRayDirection = refractRay(currentRayDirection, normal, closestMaterial.ior);
            currentRayOrigin = hitPoint + currentRayDirection * 0.001; // Offset to avoid self-intersection
        } else {
            break; // No further bounces for opaque surfaces
        }
    }

    FragColor = vec4(color, 1.0);

}