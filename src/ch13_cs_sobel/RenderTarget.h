#pragma once

#include "D3DUtil.h"

class RenderTarget {
  public:
    RenderTarget(ID3D12Device *device, int width, int height, DXGI_FORMAT fmt);
    RenderTarget(const RenderTarget &rhs) = delete;
    RenderTarget &operator=(const RenderTarget &rhs) = delete;

    ID3D12Resource *Resource() const {
        return p_render_target.Get();
    }

    constexpr int DescriptorCount() const {
        return 1;
    }

    CD3DX12_GPU_DESCRIPTOR_HANDLE Srv() const {
        return srv_gpu;
    }
    CD3DX12_CPU_DESCRIPTOR_HANDLE Rtv() const {
        return rtv_cpu;
    }

    void BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_srv_cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE h_srv_gpu,
        CD3DX12_CPU_DESCRIPTOR_HANDLE h_rtv_cpu);

    void OnResize(int new_width, int new_height);

  private:
    void BuildResource();
    void BuildDescriptors();

    ID3D12Device *device;

    int width = 0;
    int height = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    CD3DX12_CPU_DESCRIPTOR_HANDLE srv_cpu;
    CD3DX12_GPU_DESCRIPTOR_HANDLE srv_gpu;
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_cpu;

    Microsoft::WRL::ComPtr<ID3D12Resource> p_render_target;
};