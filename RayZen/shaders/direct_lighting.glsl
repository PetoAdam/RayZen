vec3 calculateLighting(vec3 hitPoint, vec3 normal, Material material, vec3 viewDir, bool includeAmbient) {
    bool dielectric = uniformShadingMode != 2 &&
        (material.transparency > 0.0 ||
         (uniformShadingMode == 0 && uniformSubsurfaceEnabled && material.subsurfaceMeanFreePath > 0.0));
    Material surface = material;
    if (dielectric) {
        surface.albedo = vec3(0.0);
        surface.metallic = 0.0;
        surface.reflectivity = 0.0;
    }
    vec3 result = includeAmbient && !dielectric ? uniformAmbientLightColor * material.albedo : vec3(0.0);
    for (int i = 0; i < numLights && i < lights.length(); ++i) {
        Light light = lights[i];
        vec3 direction;
        float attenuation;
        float maxDistance;
        if (light.positionOrDirection.w == 1.0) {
            vec3 delta = light.positionOrDirection.xyz - hitPoint;
            float distance = max(length(delta), 0.001);
            direction = delta / distance;
            attenuation = light.power / (distance * distance);
            maxDistance = distance;
        } else {
            direction = normalize(light.positionOrDirection.xyz);
            attenuation = light.power;
            maxDistance = 1e30;
        }
        float nl = max(dot(normal, direction), 0.0);
        if (nl <= 0.0) continue;
        vec3 shadowOrigin = offsetRayOrigin(hitPoint, normal, direction);
        vec3 visibility;
#ifdef RAYZEN_FRAGMENT_BACKEND
        bool blockGlass = false;
#else
        bool blockGlass = uniformCausticsEnabled && includeAmbient && !dielectric;
#endif
        if (!shadowVisibility(shadowOrigin, direction,
                              maxDistance - dot(shadowOrigin - hitPoint, direction),
                              blockGlass, visibility)) continue;
        vec3 brdf = uniformShadingMode == 2 ? material.albedo / PI :
                    opaqueBRDF(surface, normal, viewDir, direction);
        result += brdf * light.color * visibility * attenuation * nl;
    }
    return result;
}
