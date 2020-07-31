Texture2D base_map : register(t0);
Texture2D edge_map : register(t1);

SamplerState sam_point_wrap : register(s0);
SamplerState sam_point_clamp : register(s1);
SamplerState sam_linear_wrap : register(s2);
SamplerState sam_linear_clamp : register(s3);
SamplerState sam_aniso_wrap : register(s4);
SamplerState sam_aniso_clamp : register(s5);

static const float2 texc[4] = {
    float2(0.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f)
};

struct VertexOut {
    float4 pos : SV_POSITION;
    float2 texc : TEXCOORD;
};

VertexOut VS(uint vid : SV_VERTEXID) {
    VertexOut vout;
    vout.texc = texc[vid];
    vout.pos = float4(2.0f * texc[vid].x - 1.0f, 1.0f - 2.0f * texc[vid].y, 0.0f, 1.0f);
    return vout;
}

float4 PS(VertexOut pin) : SV_TARGET {
    float4 c = base_map.SampleLevel(sam_point_clamp, pin.texc, 0.0f);
    float4 e = edge_map.SampleLevel(sam_point_clamp, pin.texc, 0.0f);
    float mine = min(e.r, min(e.g, e.b));
    if (mine > 0.5f) {
        e = float4(1.0f, 1.0f, 1.0f, e.a);
    }
    return c * e;
}