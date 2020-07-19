#pragma once

#include <memory>

#include "D3DUtil.h"
#include "DXMath.h"
#include "UploadBuffer.h"

// data in cbuffer per object
struct ObjectConst {
    DirectX::XMFLOAT4X4 model = DXMath::Identity4x4();
    DirectX::XMFLOAT4X4 model_it = DXMath::Identity4x4();
    DirectX::XMFLOAT4X4 tex_transform = DXMath::Identity4x4();
};

// data in cbuffer per pass
struct PassConst {
    DirectX::XMFLOAT4X4 view = DXMath::Identity4x4();
    DirectX::XMFLOAT4X4 view_inv = DXMath::Identity4x4();
    DirectX::XMFLOAT4X4 proj = DXMath::Identity4x4();
    DirectX::XMFLOAT4X4 proj_inv = DXMath::Identity4x4();
    DirectX::XMFLOAT4X4 vp = DXMath::Identity4x4();
    DirectX::XMFLOAT4X4 vp_inv = DXMath::Identity4x4();
    DirectX::XMFLOAT3 eye = { 0.0f, 0.0f, 0.0f };
    float padding0 = 0.0f; // 128-bit align padding
    DirectX::XMFLOAT2 rt_size = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 rt_size_inv = { 0.0f, 0.0f };
    float near_z = 0.0f;
    float far_z = 0.0f;
    float delta_time = 0.0f;
    float total_time = 0.0f;
    DirectX::XMFLOAT4 ambient = { 0.0f, 0.0f, 0.0f, 1.0f };
    DirectX::XMFLOAT4 fog_color = { 0.7f, 0.7f, 0.7f, 1.0f };
    float fog_start = 25.0f;
    float fog_end = 150.0f;
    DirectX::XMFLOAT2 padding1 = { 0.0f, 0.0f }; // 128-bit align padding
    Light lights[MAX_N_LIGHT];
};

struct Vertex {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT3 norm;
    DirectX::XMFLOAT2 texc;
};

struct FrameResource {
    FrameResource(ID3D12Device *device, UINT n_pass, UINT n_obj, UINT n_mat, UINT n_wave_vertex);
    FrameResource(const FrameResource &rhs) = delete;
    FrameResource &operator=(const FrameResource &rhs) = delete;
    ~FrameResource();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> p_cmd_alloc;
    std::unique_ptr<UploadBuffer<ObjectConst>> p_obj_cb = nullptr;
    std::unique_ptr<UploadBuffer<PassConst>> p_pass_cb = nullptr;
    std::unique_ptr<UploadBuffer<MaterialConst>> p_mat_cb = nullptr;
    std::unique_ptr<UploadBuffer<Vertex>> p_wave_vb = nullptr;
    UINT64 fence = 0;
};