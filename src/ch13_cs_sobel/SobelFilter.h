#pragma once

#include "D3DUtil.h"
#include "d3dx12.h"

class SobelFilter {
  public:
    SobelFilter(ID3D12Device *device, int width, int height, DXGI_FORMAT fmt);
    SobelFilter(const SobelFilter &rhs) = delete;
    SobelFilter &operator=(const SobelFilter &rhs) = delete;

    ID3D12Resource *Resource() const {
        return p_tex.Get();
    }
    CD3DX12_GPU_DESCRIPTOR_HANDLE Srv() const {
      return srv_gpu;
    }

    constexpr int DescriptorCount() const {
        return 2;
    }

    void BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE h_gpu, int inc_size);

    void OnResize(int new_width, int new_height);

    void Execute(ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *pso, CD3DX12_GPU_DESCRIPTOR_HANDLE input);

  private:
    void BuildResources();
    void BuildDescriptors();

    ID3D12Device *device;

    int width = 0;
    int height = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    CD3DX12_CPU_DESCRIPTOR_HANDLE srv_cpu;
    CD3DX12_CPU_DESCRIPTOR_HANDLE uav_cpu;
    CD3DX12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    CD3DX12_GPU_DESCRIPTOR_HANDLE uav_gpu;

    Microsoft::WRL::ComPtr<ID3D12Resource> p_tex = nullptr;
};