#include "Wave.h"

#include <algorithm>

using namespace DirectX;

Wave::Wave(int m, int n, float dx, float dt, float speed, float damping) {
    n_row = m;
    n_col = n;
    n_vertex = m * n;
    n_triangle = 2 * (m - 1) * (n - 1);
    time_step = dt;
    spatial_step = dx;

    float d = damping * dt + 2.0f;
    float e = (speed * speed) * (dt * dt) / (dx * dx);
    k1 = (damping * dt - 2.0f) / d;
    k2 = (4.0f - 8.0f * e) / d;
    k3 = (2.0f * e) / d;

    prev_solution.resize(m * n);
    curr_solution.resize(m * n);
    normals.resize(m * n);
    tangents.resize(m * n);

    float hw = (n - 1) * dx * 0.5f;
    float hd = (m - 1) * dx * 0.5f;
    for (int i = 0; i < m; i++) {
        float z = hd - i * dx;
        for (int j = 0; j < n; j++) {
            float x = -hw + j * dx;

            prev_solution[i * n + j] = XMFLOAT3(x, 0.0f, z);
            curr_solution[i * n + j] = XMFLOAT3(x, 0.0f, z);
            normals[i * n + j] = XMFLOAT3(0.0f, 1.0f, 0.0f);
            tangents[i * n + j] = XMFLOAT3(1.0f, 0.0f, 0.0f);
        }
    }
}

void Wave::Update(float dt) {
    static float t = 0;

    // Accumulate time.
    t += dt;

    // Only update the simulation at the specified time step.
    if ( t >= time_step) {
        // Only update interior points; we use zero boundary conditions.
        // concurrency::parallel_for(1, n_row - 1, [this](int i) {
        for (int i = 1; i < n_row - 1; i++) {
            for(int j = 1; j < n_col - 1; j++) {
                // After this update we will be discarding the old previous
                // buffer, so overwrite that buffer with the new update.
                // Note how we can do this inplace (read/write to same element) 
                // because we won't need prev_ij again and the assignment happens last.

                // Note j indexes x and i indexes z: h(x_j, z_i, t_k)
                // Moreover, our +z axis goes "down"; this is just to 
                // keep consistent with our row indices going down.

                prev_solution[i * n_col + j].y = 
                    k1 * prev_solution[i * n_col + j].y +
                    k2 * curr_solution[i * n_col + j].y +
                    k3 * (curr_solution[(i + 1) * n_col + j].y + 
                          curr_solution[(i - 1) * n_col + j].y + 
                          curr_solution[i * n_col + j + 1].y + 
                          curr_solution[i * n_col + j - 1].y);
            }
        }
        // );

        // We just overwrote the previous buffer with the new data, so
        // this data needs to become the current solution and the old
        // current solution becomes the new previous solution.
        std::swap(prev_solution, curr_solution);

        t = 0.0f; // reset time

        // Compute normals using finite difference scheme.
        // concurrency::parallel_for(1, n_row - 1, [this](int i) {
        for (int i = 1; i < n_row - 1; i++) {
            for (int j = 1; j < n_col - 1; j++) {
                float l = curr_solution[i * n_col + j - 1].y;
                float r = curr_solution[i * n_col + j + 1].y;
                float t = curr_solution[(i - 1) * n_col + j].y;
                float b = curr_solution[(i + 1) * n_col + j].y;
                normals[i * n_col + j].x = -r + l;
                normals[i * n_col + j].y = 2.0f * spatial_step;
                normals[i * n_col + j].z = b - t;

                XMVECTOR n = XMVector3Normalize(XMLoadFloat3(&normals[i * n_col + j]));
                XMStoreFloat3(&normals[i * n_col + j], n);

                tangents[i * n_col + j] = XMFLOAT3(2.0f * spatial_step, r - l, 0.0f);
                XMVECTOR T = XMVector3Normalize(XMLoadFloat3(&tangents[i * n_col + j]));
                XMStoreFloat3(&tangents[i * n_col + j], T);
            }
        }
        // );
    }
}

void Wave::Disturb(int i, int j, float magnitude) {
    // Don't disturb boundaries.
    assert(i > 1 && i < n_row - 2);
    assert(j > 1 && j < n_col - 2);

    float hm = 0.5f*magnitude;

    // Disturb the ijth vertex height and its neighbors.
    curr_solution[i * n_col + j].y += magnitude;
    curr_solution[i * n_col + j + 1].y += hm;
    curr_solution[i * n_col + j - 1].y += hm;
    curr_solution[(i + 1) * n_col + j].y += hm;
    curr_solution[(i - 1) * n_col + j].y += hm;
}