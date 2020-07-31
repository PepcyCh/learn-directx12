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
};

struct VertexOut {
    float3 pos : POSITION;
};

VertexOut VS(VertexIn vin) {
    VertexOut vout;
    vout.pos = vin.pos;
    return vout;
}

struct PatchTess {
    float edge_tess[4] : SV_TessFactor;
    float inside_tess[2] : SV_InsideTessFactor;
};

PatchTess ConstHS(InputPatch<VertexOut, 4> patch, uint pid : SV_PrimitiveID) {
    PatchTess pt;

    float3 center = 0.25f * (patch[0].pos + patch[1].pos, patch[2].pos, patch[3].pos);
    center = mul(model, float4(center, 1.0f));
    float dist = distance(center, eye);

    const float d0 = 20.0f;
    const float d1 = 100.0f;
    float tess = 63.0f * saturate((d1 - dist) / (d1 - d0)) + 1.0f;

    pt.edge_tess[0] = tess;
    pt.edge_tess[1] = tess;
    pt.edge_tess[2] = tess;
    pt.edge_tess[3] = tess;

    pt.inside_tess[0] = tess;
    pt.inside_tess[1] = tess;

    return pt;
}

struct HullOut {
    float3 pos : POSITION;
};

[domain("quad")]
[partitioning("integer")]
// [partitioning("fractional_odd")]
// [partitioning("fractional_even")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(4)]
[patchconstantfunc("ConstHS")]
[maxtessfactor(64.0f)]
HullOut HS(InputPatch<VertexOut, 4> patch, uint i : SV_OutputControlPointID, uint pid : SV_PrimitiveID) {
    HullOut hout;
    hout.pos = patch[i].pos;
    return hout;
}

struct DomainOut {
    float4 pos : SV_POSITION;
    float3 pos_w : POSITION;
    float3 norm_w : NORMAL;
    float2 texc : TEXCOORD;
};

[domain("quad")]
DomainOut DS(PatchTess pt, float2 uv : SV_DomainLocation, const OutputPatch<HullOut, 4> patch) {
    DomainOut dout;

    float3 v0 = lerp(patch[0].pos, patch[1].pos, uv.x);
    float3 v1 = lerp(patch[2].pos, patch[3].pos, uv.x);
    float3 p = lerp(v0, v1, uv.y);

    p.y = 0.3f * (p.z * sin(p.x) + p.x * cos(p.z));
    float4 pos_w = mul(model, float4(p, 1.0f));
    dout.pos_w = pos_w;
    dout.pos = mul(vp, pos_w);
    dout.norm_w = normalize(float3(-0.3f * p.z * cos(p.x) - 0.3f * cos(p.z), 1.0f,
        -0.3f * sin(p.x) + 0.3f * p.x * sin(p.z)));
    dout.texc = float2(p.x / 10.0f, p.z / 10.0f);

    return dout;
}

float4 PS(DomainOut pin) : SV_TARGET {
    float4 albedo = diffuse_map.Sample(sam_aniso_wrap, pin.texc) * g_albedo;
    pin.norm_w = normalize(pin.norm_w);
    float3 view_w = eye - pin.pos_w;
    float view_dist = length(view_w);
    float3 view = view_w / view_dist;
    float4 ambient = g_ambient * albedo;
    float shiniess = 1.0f - roughness;
    Material mat = { albedo, fresnel_r0, shiniess };
    float4 light_res = ComputeLight(lights, mat, pin.pos_w, pin.norm_w, view);
    float4 res = light_res + ambient;
    res.a = albedo.a;
    return res;
}