#pragma once

#include "D3DUtil.h"

class BlurFilter {
  public:
    BlurFilter(ID3D12Device *device, int width, int height, DXGI_FORMAT fmt);
    BlurFilter(const BlurFilter &rhs) = delete;
    BlurFilter &operator=(const BlurFilter &rhs) = delete;

    ID3D12Resource *Output() {
        return p_blur0.Get();
    }

    constexpr int DescriptorCount() const {
        return 4;
    }

    void BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE h_gpu, int inc_size);

    void OnResize(int new_width, int new_height);

    void Execute(ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *hori_pso, ID3D12PipelineState *vert_pso, ID3D12Resource *input, int n_blur);

  private:
    std::vector<float> GaussWeights(float sigma) const;

    void BuildDescriptors();
    void BuildResources();

    inline static const int kMaxBlurRadius = 5;

    ID3D12Device *device;

    int width = 0;
    int height = 0;

    DXGI_FORMAT fmt = DXGI_FORMAT_R8G8B8A8_UNORM;

    CD3DX12_CPU_DESCRIPTOR_HANDLE blur0_srv_cpu;
    CD3DX12_CPU_DESCRIPTOR_HANDLE blur0_uav_cpu;
    CD3DX12_CPU_DESCRIPTOR_HANDLE blur1_srv_cpu;
    CD3DX12_CPU_DESCRIPTOR_HANDLE blur1_uav_cpu;

    CD3DX12_GPU_DESCRIPTOR_HANDLE blur0_srv_gpu;
    CD3DX12_GPU_DESCRIPTOR_HANDLE blur0_uav_gpu;
    CD3DX12_GPU_DESCRIPTOR_HANDLE blur1_srv_gpu;
    CD3DX12_GPU_DESCRIPTOR_HANDLE blur1_uav_gpu;

    Microsoft::WRL::ComPtr<ID3D12Resource> p_blur0 = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> p_blur1 = nullptr;
};