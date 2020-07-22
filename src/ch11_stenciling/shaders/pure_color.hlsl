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
    float4 res = float4(1.0f, 1.0f, 1.0f, 1.0f);
    return res;
}