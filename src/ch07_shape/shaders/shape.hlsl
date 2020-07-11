cbuffer obj_cb : register(b0) {
    float4x4 model;
};

cbuffer pass_cb : register(b1) {
    float4x4 view;
    float4x4 view_inv;
    float4x4 proj;
    float4x4 proj_inv;
    float4x4 vp;
    float4x4 vp_inv;
    float3 eye;
}

struct VertexIn {
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct VertexOut {
    float4 pos : SV_POSITION;
    float4 color : COLOR;
};

VertexOut VS(VertexIn vin) {
    VertexOut vout;
    // vout.pos = mul(float4(vin.pos, 1.0f), model);
    // vout.pos = mul(vout.pos, vp);
    vout.pos = mul(model, float4(vin.pos, 1.0f));
    vout.pos = mul(vp, vout.pos);
    vout.color = vin.color;
    return vout;
}

float4 PS(VertexOut pin) : SV_TARGET {
    return pin.color;
}