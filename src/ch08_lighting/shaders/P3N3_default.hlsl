#ifndef N_DIR_LIGHTS
#define N_DIR_LIGHTS 1
#endif

#ifndef N_POINT_LIGHTS
#define N_POINT_LIGHTS 0
#endif

#ifndef N_SPOT_LIGHTS
#define N_SPOT_LIGHTS 0
#endif

#include "light.hlsl"

cbuffer obj_cb : register(b0) {
    float4x4 model;
    float4x4 model_it;
};

cbuffer mat_cb : register(b1) {
    float4 albedo;
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
    Light lights[MAX_N_LIGHTS];   
}

struct VertexIn {
    float3 pos : POSITION;
    float3 norm : NORMAL;
};

struct VertexOut {
    float4 pos : SV_POSITION;
    float3 pos_w : POSITION;
    float3 norm_w : NORMAL;
};

VertexOut VS(VertexIn vin) {
    VertexOut vout;
    float4 pos_w = mul(model, float4(vin.pos, 1.0f));
    vout.pos_w = pos_w.xyz;
    vout.pos = mul(vp, pos_w);
    vout.norm_w = mul((float3x3) model_it, vin.norm);
    return vout;
}

float4 PS(VertexOut pin) : SV_TARGET {
    pin.norm_w = normalize(pin.norm_w);
    float3 view = normalize(eye - pin.pos_w);
    float4 ambient = g_ambient * albedo;
    float shiniess = 1.0f - roughness;
    Material mat = { albedo, fresnel_r0, shiniess };
    float4 light_res = ComputeLight(lights, mat, pin.pos_w, pin.norm_w, view);
    float4 res = light_res + ambient;
    res.a = albedo.a;
    return res;
}