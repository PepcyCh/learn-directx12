#pragma once

#include "D3DUtil.h"

struct MeshGeometryEx {
    std::string name;

    // number of vertex buffers
    inline static const int kVertexBufferCnt = 2;

    // x_cpu holds data
    Microsoft::WRL::ComPtr<ID3DBlob> vb_cpu[kVertexBufferCnt] = { nullptr, nullptr };
    Microsoft::WRL::ComPtr<ID3DBlob> ib_cpu = nullptr;

    // x_gpu provide data for dx
    Microsoft::WRL::ComPtr<ID3D12Resource> vb_gpu[kVertexBufferCnt] = { nullptr, nullptr };
    Microsoft::WRL::ComPtr<ID3D12Resource> ib_gpu = nullptr;

    // uploader as intermediate buffer
    Microsoft::WRL::ComPtr<ID3D12Resource> vb_uploader[kVertexBufferCnt] = { nullptr, nullptr };
    Microsoft::WRL::ComPtr<ID3D12Resource> ib_uploader = nullptr;

    UINT vb_stride[kVertexBufferCnt] = { 0, 0 };
    UINT vb_size[kVertexBufferCnt] = { 0, 0 };
    DXGI_FORMAT index_fmt = DXGI_FORMAT_R16_UINT;
    UINT ib_size = 0;

    std::unordered_map<std::string, SubmeshGeometry> draw_args;

    D3D12_VERTEX_BUFFER_VIEW VertexBufferView(int i) const {
        D3D12_VERTEX_BUFFER_VIEW vbv;
        vbv.BufferLocation = vb_gpu[i]->GetGPUVirtualAddress();
        vbv.SizeInBytes = vb_size[i];
        vbv.StrideInBytes = vb_stride[i];
        return vbv;
    }
    D3D12_INDEX_BUFFER_VIEW IndexBufferView() const {
        D3D12_INDEX_BUFFER_VIEW ibv;
        ibv.BufferLocation = ib_gpu->GetGPUVirtualAddress();
        ibv.Format = index_fmt;
        ibv.SizeInBytes = ib_size;
        return ibv;
    }

    void DisposeUploader() {
        for (int i = 0; i < kVertexBufferCnt; i++) {
            vb_uploader[i] = nullptr;
        }
        ib_uploader = nullptr;
    }
};
