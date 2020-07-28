Texture2D input : register(t0);
RWTexture2D<float4> output : register(u0);

float Luminance(float3 color) {
    return dot(color, float3(0.299f, 0.587f, 0.114f));
}

[numthreads(16, 16, 1)]
void SobelCS(int3 dtid : SV_DispatchThreadID) {
    float4 c[3][3];
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            int2 xy = dtid.xy + int2(i - 1, j - 1);
            c[i][j] = input[xy];
        }
    }

    // For each color channel, estimate partial x derivative using Sobel scheme.
	float4 Gx = -1.0f * c[0][0] - 2.0f * c[1][0] - 1.0f * c[2][0] + 1.0f * c[0][2] + 2.0f * c[1][2] + 1.0f * c[2][2];

	// For each color channel, estimate partial y derivative using Sobel scheme.
	float4 Gy = -1.0f * c[2][0] - 2.0f * c[2][1] - 1.0f * c[2][1] + 1.0f * c[0][0] + 2.0f * c[0][1] + 1.0f * c[0][2];

	// Gradient is (Gx, Gy). For each color channel, compute magnitude to get maximum rate of change.
	float4 mag = sqrt(Gx * Gx + Gy * Gy);

	// Make edges black, and nonedges white.
	mag = 1.0f - saturate(Luminance(mag.rgb));
	output[dtid.xy] = mag;
}