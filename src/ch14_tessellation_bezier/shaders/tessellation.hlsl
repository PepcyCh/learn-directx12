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

PatchTess ConstHS(InputPatch<VertexOut, 16> patch, uint pid : SV_PrimitiveID) {
    PatchTess pt;

    float tess = 25.0f;

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
[outputtopology("triangle_cw")]
[outputcontrolpoints(16)]
[patchconstantfunc("ConstHS")]
[maxtessfactor(64.0f)]
HullOut HS(InputPatch<VertexOut, 16> patch, uint i : SV_OutputControlPointID, uint pid : SV_PrimitiveID) {
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

float4 BernsteinBasic(float t) {
    float it = 1.0f - t;
    return float4(it * it * it, 3.0f * it * it * t, 3.0f * it * t * t, t * t * t);
}

float4 DBernsteinBasic(float t) {
    float it = 1.0f - t;
    return float4(-3 * it * it, 3 * it * it - 6 * it * t, 6 * t * it - 3 * t * t, 3 * t * t);
}

float3 CubizBezierSum(const OutputPatch<HullOut, 16> patch, float4 basic_u, float4 basic_v) {
    float3 sum = float3(0.0f, 0.0f, 0.0f);
    sum = basic_v.x *(basic_u.x * patch[0].pos + basic_u.y * patch[1].pos + basic_u.z * patch[2].pos +
        basic_u.w * patch[3].pos);
    sum += basic_v.y *(basic_u.x * patch[4].pos + basic_u.y * patch[5].pos + basic_u.z * patch[6].pos +
        basic_u.w * patch[7].pos);
    sum += basic_v.z *(basic_u.x * patch[8].pos + basic_u.y * patch[9].pos + basic_u.z * patch[10].pos +
        basic_u.w * patch[11].pos);
    sum += basic_v.w *(basic_u.x * patch[12].pos + basic_u.y * patch[13].pos + basic_u.z * patch[14].pos +
        basic_u.w * patch[15].pos);
    return sum;
}

[domain("quad")]
DomainOut DS(PatchTess pt, float2 uv : SV_DomainLocation, const OutputPatch<HullOut, 16> patch) {
    DomainOut dout;

    float4 basic_u = BernsteinBasic(uv.x);
    float4 basic_v = BernsteinBasic(uv.y);

    float4 d_basic_u = DBernsteinBasic(uv.x);
    float4 d_basic_v = DBernsteinBasic(uv.y);

    float3 pos = CubizBezierSum(patch, basic_u, basic_v);
    float3 dpdu = CubizBezierSum(patch, d_basic_u, basic_v);
    float3 dpdv = CubizBezierSum(patch, basic_u, d_basic_v);

    float4 pos_w = mul(model, float4(pos, 1.0f));
    dout.pos_w = pos_w;
    dout.pos = mul(vp, pos_w);
    dout.norm_w = cross(normalize(dpdu), normalize(dpdv));
    dout.texc = uv;

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