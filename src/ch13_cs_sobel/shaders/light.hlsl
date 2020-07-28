#define MAX_N_LIGHTS 16

struct Light {
    float3 strength;
    float falloff_start;
    float3 direction;
    float falloff_end;
    float3 position;
    float spot_power;
};

struct Material {
    float4 albedo;
    float3 fresnel_r0;
    float shininess;
};

float CalcAttenuation(float d, float falloff_start, float falloff_end) {
    // saturate(x) = clamp(x, 0, 1)
    return saturate((falloff_end - d) / (falloff_end - falloff_start));
}

float3 SchlickFresnel(float3 r0, float3 normal, float3 light) {
    float cosine = max(dot(normal, light), 0.0f);
    float f0 = 1.0f - cosine;
    float3 reflect_percent = r0 + (1.0f - r0) * (f0 * f0 * f0 * f0 * f0);
    return reflect_percent;
}

float3 BlinnPhong(float3 strength, float3 light, float3 normal, float3 view, Material mat) {
    float m = mat.shininess * 256.0f;
    float3 halfway = normalize(view + light);

    float ks = pow(max(dot(halfway, normal), 0.0f), m);
    if (ks <= 0.1f) {
        ks = 0.0f;
    } else if (ks <= 0.8) {
        ks = 0.5f;
    } else {
        ks = 0.8f;
    }
    float roughness_factor = (m + 8.0f) * ks / 8.0f;
    float3 fresnel_factor = SchlickFresnel(mat.fresnel_r0, halfway, light);

    float3 spec_albedo = fresnel_factor * roughness_factor;
    spec_albedo /= (spec_albedo + 1.0f);

    return (mat.albedo.rgb + spec_albedo) * strength;
}

float3 ComputeDirectionalLight(Light L, Material mat, float3 pos, float3 normal, float3 view) {
    float3 light = -L.direction;
    float kd = max(dot(normal, light), 0.0f);
    if (kd <= 0.1f) {
        kd = 0.4f;
    } else if (kd <= 0.5) {
        kd = 0.6f;
    } else {
        kd = 1.0f;
    }
    float3 strength = L.strength * kd;
    return BlinnPhong(strength, light, normal, view, mat);
}

float3 ComputePointLight(Light L, Material mat, float3 pos, float3 normal, float3 view) {
    float3 light = L.position - pos;
    float d = length(light);
    if (d > L.falloff_end) {
        return 0.0f;
    }
    light /= d;
    float ndotl = max(dot(normal, light), 0.0f);
    float3 strength = L.strength * ndotl * CalcAttenuation(d, L.falloff_start, L.falloff_end);
    return BlinnPhong(strength, light, normal, view, mat);
}

float3 ComputeSpotLight(Light L, Material mat, float3 pos, float3 normal, float3 view) {
    float3 light = L.position - pos;
    float d = length(light);
    if (d > L.falloff_end) {
        return 0.0f;
    }
    light /= d;
    float ndotl = max(dot(normal, light), 0.0f);
    float3 strength = L.strength * ndotl * CalcAttenuation(d, L.falloff_start, L.falloff_end);
    float spot_factor = pow(max(dot(-light, L.direction), 0.0f), L.spot_power);
    strength *= spot_factor;
    return BlinnPhong(strength, light, normal, view, mat);
}

float4 ComputeLight(Light lights[MAX_N_LIGHTS], Material mat, float3 pos, float3 normal, float3 view) {
    float3 res = 0.0f;

    int i = 0;
#if (N_DIR_LIGHTS > 0)
    for (i = 0; i < N_DIR_LIGHTS; i++) {
        res += ComputeDirectionalLight(lights[i], mat, pos, normal, view);
    }
#endif
#if (N_POINT_LIGHTS > 0)
    for (i = N_DIR_LIGHTS; i < N_DIR_LIGHTS + N_POINT_LIGHTS; i++) {
        res += ComputePointLight(lights[i], mat, pos, normal, view);
    }
#endif
#if (N_SPOT_LIGHTS > 0)
    for (i = N_DIR_LIGHTS + N_POINT_LIGHTS; i < N_DIR_LIGHTS + N_POINT_LIGHTS + N_SPOT_LIGHTS; i++) {
        res += ComputeSpotLight(lights[i], mat, pos, normal, view);
    }
#endif

    return float4(res, 0.0f);
}