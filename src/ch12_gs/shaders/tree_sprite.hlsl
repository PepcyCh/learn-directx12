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

Texture2DArray tree_maps : register(t0);

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
    float2 size : SIZE;
};

struct VertexOut {
    float3 pos : POSITION;
    float2 size : SIZE;
};

struct GeometryOut {
    float4 pos : SV_POSITION;
    float3 pos_w : POSITION;
    float3 norm_w : NORMAL;
    float2 texc : TEXCOORD;
    uint prim_id : SV_PrimitiveID;
};

VertexOut VS(VertexIn vin) {
    VertexOut vout;
    vout.pos = vin.pos;
    vout.size = vin.size;
    return vout;
}

[maxvertexcount(4)]
void GS(point VertexOut gin[1], uint prim_id : SV_PrimitiveID,
        inout TriangleStream<GeometryOut> gout_stream) {
    float3 up = float3(0.0f, 1.0f, 0.0f);
    float3 look = eye - gin[0].pos;
    look.y = 0.0f;
    look = normalize(look);
    float3 right = cross(look, up);

    float hw = gin[0].size.x * 0.5f;
    float hh = gin[0].size.y * 0.5f;

    float4 pos[4];
    pos[0] = float4(gin[0].pos + hw * right + hh * up, 1.0f);
    pos[1] = float4(gin[0].pos + hw * right - hh * up, 1.0f);
    pos[2] = float4(gin[0].pos - hw * right + hh * up, 1.0f);
    pos[3] = float4(gin[0].pos - hw * right - hh * up, 1.0f);

    float2 texc[4] = {
        { 0.0f, 0.0f },
        { 0.0f, 1.0f },
        { 1.0f, 0.0f },
        { 1.0f, 1.0f }
    };

    GeometryOut gout;
    [unroll]
    for (int i = 0; i < 4; i++) {
        gout.pos = mul(vp, pos[i]);
        gout.pos_w = pos[i].xyz;
        gout.norm_w = look;
        gout.texc = texc[i];
        gout.prim_id = prim_id;
        gout_stream.Append(gout);
    }
}

float4 PS(GeometryOut pin) : SV_TARGET {
    float3 uvw = float3(pin.texc, pin.prim_id % 3);
    float4 albedo = tree_maps.Sample(sam_aniso_wrap, uvw) * g_albedo;
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