#pragma once

#include <memory>

#include "D3DUtil.h"
#include "DXMath.h"
#include "UploadBuffer.h"

// data in cbuffer per object
struct ObjectConst {
    DirectX::XMFLOAT4X4 model = DXMath::Identity4x4();
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
    DirectX::XMFLOAT2 rt_size = { 0.0f, 0.0f };
    DirectX::XMFLOAT2 rt_size_inv = { 0.0f, 0.0f };
    float near_z = 0.0f;
    float far_z = 0.0f;
    float delta_time = 0.0f;
    float total_time = 0.0f;
};

struct Vertex {
    DirectX::XMFLOAT3 pos;
    DirectX::XMFLOAT4 color;
};

struct FrameResource {
    FrameResource(ID3D12Device *device, UINT n_pass, UINT n_obj);
    FrameResource(const FrameResource &rhs) = delete;
    FrameResource &operator=(const FrameResource &rhs) = delete;
    ~FrameResource();

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> p_cmd_alloc;
    std::unique_ptr<UploadBuffer<ObjectConst>> p_obj_cb = nullptr;
    std::unique_ptr<UploadBuffer<PassConst>> p_pass_cb = nullptr;
    UINT64 fence = 0;
};