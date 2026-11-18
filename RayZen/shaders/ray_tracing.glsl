struct RealtimeRay {
    vec3 origin;
    vec3 direction;
    vec3 weight;
    vec3 mediumTransmittance;
    int mediumInstance;
    float mediumIor;
    int depth;
};

vec3 traceRealtime(Ray primary, vec2 seed) {
    RealtimeRay pending[4];
    int count = 1;
    pending[0] = RealtimeRay(primary.origin, primary.direction, vec3(1.0), vec3(1.0), -1, 1.0, 0);
    vec3 result = vec3(0.0);
    while (count > 0) {
        RealtimeRay path = pending[--count];
        for (int depth = path.depth; depth < 4; ++depth) {
            float distance;
            vec3 point, geometricNormal;
            int materialIndex, instance;
            vec2 uv;
            bool hit = traverseTLAS(Ray(path.origin, path.direction), distance, point,
                                    geometricNormal, materialIndex, instance, uv);
            if (path.mediumInstance >= 0) {
                if (!hit || instance != path.mediumInstance) break;
                path.weight *= pow(path.mediumTransmittance, vec3(distance));
            }
            if (!hit) {
                float t = 0.5 * (path.direction.y + 1.0);
                result += path.weight * mix(vec3(0.15, 0.25, 0.45), vec3(0.5, 0.7, 1.0), t);
                break;
            }
            Material material = materials[materialIndex];
            applyBaseColorTexture(material, uv);
            vec3 viewDir = -path.direction;
            vec3 normal = dot(geometricNormal, viewDir) >= 0.0 ? geometricNormal : -geometricNormal;
            if (path.mediumInstance < 0)
                result += path.weight * calculateLighting(point, normal, material, viewDir, depth == 0);
            if (depth == 3) break;
            if (material.transparency > 0.0) {
                bool entering = path.mediumInstance < 0;
                float nextIor = entering ? max(material.ior, 0.001) : 1.0;
                float fresnel = dielectricFresnel(clamp(dot(viewDir, normal), 0.0, 1.0),
                                                   path.mediumIor, nextIor);
                vec3 transmitted = refract(path.direction, normal, path.mediumIor / nextIor);
                if (fresnel > 0.001 && count < 4) {
                    RealtimeRay reflected = path;
                    reflected.direction = reflect(path.direction, normal);
                    reflected.origin = offsetRayOrigin(point, geometricNormal, reflected.direction);
                    reflected.weight *= fresnel;
                    reflected.depth = depth + 1;
                    pending[count++] = reflected;
                }
                if (fresnel >= 1.0 || dot(transmitted, transmitted) < 1e-8) break;
                path.weight *= 1.0 - fresnel;
                path.direction = normalize(transmitted);
                path.mediumInstance = entering ? instance : -1;
                path.mediumIor = nextIor;
                if (entering)
                    path.mediumTransmittance = clamp(material.albedo * material.transparency,
                                                     vec3(0.0001), vec3(1.0));
            } else {
                float rough = clamp(material.roughness, MIN_ROUGHNESS, 1.0);
                float alpha = rough * rough;
                vec2 sampleSeed = seed + vec2(float(depth) * 31.7, float(depth) * 79.1);
                float u = rand(sampleSeed + vec2(11.13, 5.19));
                float phi = 2.0 * PI * rand(sampleSeed + vec2(3.71, 17.23));
                float cosTheta = sqrt((1.0 - u) / (1.0 + (alpha * alpha - 1.0) * u));
                float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
                vec3 up = abs(normal.y) < 0.99 ? vec3(0,1,0) : vec3(1,0,0);
                vec3 tangent = normalize(cross(up, normal));
                vec3 halfVector = normalize(tangent * (sinTheta * cos(phi)) +
                    cross(normal, tangent) * (sinTheta * sin(phi)) + normal * cosTheta);
                float vh = max(dot(viewDir, halfVector), 0.0);
                float nv = max(dot(normal, viewDir), 0.0);
                path.direction = reflect(-viewDir, halfVector);
                float nl = max(dot(normal, path.direction), 0.0);
                if (nl <= 0.0 || vh <= 0.0 || nv <= 0.0) break;
                float k = (rough + 1.0) * (rough + 1.0) / 8.0;
                float G = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
                path.weight *= fresnelSchlick(vh, materialF0(material)) *
                               (G * vh / max(nv * cosTheta, 1e-6));
            }
            if (max(path.weight.r, max(path.weight.g, path.weight.b)) < 0.001) break;
            path.origin = offsetRayOrigin(point, geometricNormal, path.direction);
        }
    }
    return result;
}
