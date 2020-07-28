cbuffer update_cb : register(b0) {
    float k0;
    float k1;
    float k2;
    float disturb_mag;
    int2 disturb_ind;
}

RWTexture2D<float> prev_solution : register(u0);
RWTexture2D<float> curr_solution : register(u1);
RWTexture2D<float> next_solution : register(u2);

[numthreads(16, 16, 1)]
void UpdateWaveCS(int3 dtid : SV_DispatchThreadID) {
    int x = dtid.x;
    int y = dtid.y;

    next_solution[int2(x, y)] = k0 * prev_solution[int2(x, y)] + k1 * curr_solution[int2(x, y)] +
        k2 * (curr_solution[int2(x - 1, y)] + curr_solution[int2(x + 1, y)] +
        curr_solution[int2(x, y - 1)] + curr_solution[int2(x, y + 1)]);
}

[numthreads(1, 1, 1)]
void DisturbCS(int3 dtid : SV_DispatchThreadID) {
    int x = disturb_ind.x;
    int y = disturb_ind.y;
    float half_mag = 0.5f * disturb_mag;

    next_solution[int2(x, y)] += disturb_mag;
    next_solution[int2(x - 1, y)] += half_mag;
    next_solution[int2(x + 1, y)] += half_mag;
    next_solution[int2(x, y - 1)] += half_mag;
    next_solution[int2(x, y + 1)] += half_mag;
}