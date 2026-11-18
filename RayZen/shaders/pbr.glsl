vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(1.0 - cosTheta, 5.0);
}

vec3 materialF0(Material m) {
    float ior = max(m.ior, 0.001);
    float dielectric = pow((ior - 1.0) / (ior + 1.0), 2.0);
    dielectric = clamp(dielectric * (1.0 + clamp(m.reflectivity, 0.0, 1.0)), 0.0, 1.0);
    return mix(vec3(dielectric), m.albedo, clamp(m.metallic, 0.0, 1.0));
}

vec3 opaqueBRDF(Material m, vec3 n, vec3 v, vec3 l) {
    float nv = max(dot(n, v), 0.0), nl = max(dot(n, l), 0.0);
    if (nv <= 0.0 || nl <= 0.0 || length(v + l) < 1e-6) return vec3(0.0);
    vec3 h = normalize(v + l);
    float nh = max(dot(n, h), 0.0), lh = max(dot(l, h), 0.0);
    float rough = clamp(m.roughness, MIN_ROUGHNESS, 1.0);
    float alpha = rough * rough, a2 = alpha * alpha;
    float d = nh * nh * (a2 - 1.0) + 1.0;
    float D = a2 / max(PI * d * d, 1e-20);
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float G = (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
    vec3 F = fresnelSchlick(lh, materialF0(m));
    float fd90 = 0.5 + 2.0 * rough * lh * lh;
    float diffuse = (1.0 + (fd90 - 1.0) * pow(1.0 - nl, 5.0)) *
                    (1.0 + (fd90 - 1.0) * pow(1.0 - nv, 5.0));
    return (1.0 - F) * (1.0 - clamp(m.metallic, 0.0, 1.0)) * m.albedo * (diffuse / PI) +
           F * D * G / max(4.0 * nv * nl, 1e-6);
}
