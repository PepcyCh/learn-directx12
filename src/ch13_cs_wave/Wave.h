#pragma once

#include <vector>

#include <wrl.h>
#include <d3d12.h>
#include <DirectXMath.h>

#include "d3dx12.h"
#include "Timer.h"

class Wave {
  public:
    Wave(ID3D12Device *device, ID3D12GraphicsCommandList *cmd_list, int m, int n, float dx, float dt,
        float speed, float damping);
    Wave(const Wave &rhs) = delete;
    Wave &operator=(const Wave &rhs) = delete;

    int RowCount() const {
        return n_row;
    }
    int ColumnCount() const {
        return n_col;
    }
    int VertexCount() const {
        return n_vertex;
    }
    int TriangleCount() const {
        return n_triangle;
    }
    float Width() const {
        return n_col * spatial_step;
    }
    float Depth() const {
        return n_row * spatial_step;
    }
    float SpatialStep() const {
        return spatial_step;
    }
    
    CD3DX12_GPU_DESCRIPTOR_HANDLE DisplacementMap() const {
        return curr_srv;
    }

    int DescriptorCount() const {
        return 6;
    }

    void BuildResources(ID3D12GraphicsCommandList *cmd_list);
    void BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE h_gpu, int inc_size);

    void Update(const Timer &timer, ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *pso);
    void Disturb(ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *pso, int i, int j, float magnitude);

  private:
    int n_row = 0;
    int n_col = 0;
    int n_vertex = 0;
    int n_triangle = 0;

    float k[3];

    float time_step = 0.0f;
    float spatial_step = 0.0f;

    ID3D12Device *device;

    CD3DX12_GPU_DESCRIPTOR_HANDLE prev_srv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE curr_srv;
    CD3DX12_GPU_DESCRIPTOR_HANDLE next_srv;

    CD3DX12_GPU_DESCRIPTOR_HANDLE prev_uav;
    CD3DX12_GPU_DESCRIPTOR_HANDLE curr_uav;
    CD3DX12_GPU_DESCRIPTOR_HANDLE next_uav;

    Microsoft::WRL::ComPtr<ID3D12Resource> prev_solution;
    Microsoft::WRL::ComPtr<ID3D12Resource> curr_solution;
    Microsoft::WRL::ComPtr<ID3D12Resource> next_solution;

    Microsoft::WRL::ComPtr<ID3D12Resource> prev_uploader;
    Microsoft::WRL::ComPtr<ID3D12Resource> curr_uploader;
};