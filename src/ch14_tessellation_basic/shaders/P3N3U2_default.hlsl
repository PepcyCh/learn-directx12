#ifndef N_DIR_LIGHTS
#define N_DIR_LIGHTS 3
#endif

#ifndef N_POINT_LIGHTS
#define N_POINT_LIGHTS 0
#endif

#ifndef N_SPOT_LIGHTS
#define N_SPOT_LIGHTS 0
#endif

#include "light.hlsl"

Texture2D diffuse_map : register(t0);

SamplerState sam_point_wrap : register(s0);
SamplerState sam_point_clamp : register(s1);
SamplerState sam_linear_wrap : register(s2);
SamplerState sam_linear_clamp : register(s3);
SamplerState sam_aniso_wrap : register(s4);
SamplerState sam_aniso_clamp : register(s5);

cbuffer obj_cb : register(b0) {
    float4x4 model;
    float4x4 model_it;
    float4x4 tex_transform;
};

cbuffer mat_cb : register(b1) {
    float4 g_albedo;
    float3 fresnel_r0;
    float roughness;
    float4x4 mat_transform;
}

cbuffer pass_cb : register(b2) {
    float4x4 view;
    float4x4 view_inv;
    float4x4 proj;
    float4x4 proj_inv;
    float4x4 vp;
    float4x4 vp_inv;
    float3 eye;
    float pass_cb_padding0;
    float2 rt_size;
    float2 rt_size_inv;
    float near_z;
    float far_z;
    float delta_time;
    float total_time;
    float4 g_ambient;
    float4 fog_color;
    float fog_start;
    float fog_end;
    float2 padd_cb_padding1;
    Light lights[MAX_N_LIGHTS];   
}

struct VertexIn {
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float2 texc : TEXCOORD;
};

struct VertexOut {
    float4 pos : SV_POSITION;
    float3 pos_w : POSITION;
    float3 norm_w : NORMAL;
    float2 texc : TEXCOORD;
};

VertexOut VS(VertexIn vin) {
    VertexOut vout;
    float4 pos_w = mul(model, float4(vin.pos, 1.0f));
    vout.pos_w = pos_w.xyz;
    vout.pos = mul(vp, pos_w);
    vout.norm_w = mul((float3x3) model_it, vin.norm);
    vout.texc = mul(mat_transform, mul(tex_transform, float4(vin.texc, 0.0f, 1.0f)));
    return vout;
}

float4 PS(VertexOut pin) : SV_TARGET {
    float4 albedo = diffuse_map.Sample(sam_aniso_wrap, pin.texc) * g_albedo;
#ifdef ALPHA_TEST
    clip(albedo.a - 0.1f);
#endif
    pin.norm_w = normalize(pin.norm_w);
    float3 view_w = eye - pin.pos_w;
    float view_dist = length(view_w);
    float3 view = view_w / view_dist;
    float4 ambient = g_ambient * albedo;
    float shiniess = 1.0f - roughness;
    Material mat = { albedo, fresnel_r0, shiniess };
    float4 light_res = ComputeLight(lights, mat, pin.pos_w, pin.norm_w, view);
    float4 res = light_res + ambient;
#ifdef FOG
    // float fog_coe = saturate((view_dist - fog_start) / (fog_end - fog_start));
    float fog_coe = 1.0f - exp(-view_dist / 100.0f);
    res = lerp(res, fog_color, fog_coe);
#endif
    res.a = albedo.a;
    return res;
}