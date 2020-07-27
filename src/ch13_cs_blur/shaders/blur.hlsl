cbuffer blur_cb : register(b0) {
    int blur_rad;
    float w0;
    float w1;
    float w2;
    float w3;
    float w4;
    float w5;
    float w6;
    float w7;
    float w8;
    float w9;
    float w10;
}

static const int kMaxBlurRadius = 5;

Texture2D input : register(t0);
RWTexture2D<float4> output : register(u0);

#define N 256
#define kCacheSize (N + kMaxBlurRadius * 2)
groupshared float4 cache[kCacheSize];

[numthreads(N, 1, 1)]
void HoriBlurCS(int3 gtid : SV_GroupThreadID, int3 dtid : SV_DispatchThreadID) {
    float w[11] = { w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10 };
    // d3d12book use Texture2D::Length to get size of texture2D, but I didn't find it in doc,
    // although I can still compile the shader.
    // there is a 'X' in the doc of Texture2D::GetDimensions(out w, out h) - 'This function is
    // supported for the following types of shaders:' - Pixel & Compute, but I can use it in CS.
    // I think the 'X' means '√' ...

    int W, H;
    input.GetDimensions(W, H);

    if (gtid.x < blur_rad) {
        int x = max(dtid.x - blur_rad, 0);
        cache[gtid.x] = input[int2(x, dtid.y)];
    } else if (gtid.x + blur_rad >= N) {
        int x = min(dtid.x + blur_rad, W - 1);
        cache[gtid.x + 2 * blur_rad] = input[int2(x, dtid.y)];
    }
    cache[gtid.x + blur_rad] = input[min(dtid.xy, int2(W - 1, H - 1))];

    GroupMemoryBarrierWithGroupSync();

    float4 blur_color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 11; i++) {
        blur_color += w[i] * cache[gtid.x + i];
    }
    output[dtid.xy] = blur_color;
}

[numthreads(1, N, 1)]
void VertBlurCS(int3 gtid : SV_GroupThreadID, int3 dtid : SV_DispatchThreadID) {
    float w[11] = { w0, w1, w2, w3, w4, w5, w6, w7, w8, w9, w10 };
    int W, H;
    input.GetDimensions(W, H);

    if (gtid.y < blur_rad) {
        int y = max(dtid.y - blur_rad, 0);
        cache[gtid.y] = input[int2(dtid.x, y)];
    } else if (gtid.y + blur_rad >= N) {
        int y = min(dtid.y + blur_rad, H - 1);
        cache[gtid.y + 2 * blur_rad] = input[int2(dtid.x, y)];
    }
    cache[gtid.y + blur_rad] = input[min(dtid.xy, int2(W - 1, H - 1))];

    GroupMemoryBarrierWithGroupSync();

    float4 blur_color = float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 11; i++) {
        blur_color += w[i] * cache[gtid.y + i];
    }
    output[dtid.xy] = blur_color;
}