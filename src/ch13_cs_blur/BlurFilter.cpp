#include "BlurFilter.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

BlurFilter::BlurFilter(ID3D12Device *device, int width, int height, DXGI_FORMAT fmt) :
        device(device), width(width), height(height), fmt(fmt) {
    BuildResources();
}

void BlurFilter::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE h_gpu,
        int inc_size) {
    blur0_srv_cpu = h_cpu;
    blur0_uav_cpu = h_cpu.Offset(1, inc_size);
    blur1_srv_cpu = h_cpu.Offset(1, inc_size);
    blur1_uav_cpu = h_cpu.Offset(1, inc_size);
    
    blur0_srv_gpu = h_gpu;
    blur0_uav_gpu = h_gpu.Offset(1, inc_size);
    blur1_srv_gpu = h_gpu.Offset(1, inc_size);
    blur1_uav_gpu = h_gpu.Offset(1, inc_size);

    BuildDescriptors();
}

void BlurFilter::OnResize(int new_width, int new_height) {
    if (new_width != width || new_height != height) {
        width = new_width;
        height = new_height;

        BuildResources();
        BuildDescriptors();
    }
}

void BlurFilter::Execute(ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *hori_pso, ID3D12PipelineState *vert_pso, ID3D12Resource *input, int n_blur) {
    auto weights = GaussWeights(2.5f);
    int blur_rad = weights.size() / 2;

    cmd_list->SetComputeRootSignature(rt_sig);

    cmd_list->SetComputeRoot32BitConstant(0, blur_rad, 0);
    cmd_list->SetComputeRoot32BitConstants(0, weights.size(), weights.data(), 1);

    D3D12_RESOURCE_BARRIER input_rt2cpsrc = CD3DX12_RESOURCE_BARRIER::Transition(input,
        D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd_list->ResourceBarrier(1, &input_rt2cpsrc);
    D3D12_RESOURCE_BARRIER blur0_read2cpdst = CD3DX12_RESOURCE_BARRIER::Transition(p_blur0.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd_list->ResourceBarrier(1, &blur0_read2cpdst);
    cmd_list->CopyResource(p_blur0.Get(), input);

    D3D12_RESOURCE_BARRIER blur0_cpdst2read = CD3DX12_RESOURCE_BARRIER::Transition(p_blur0.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmd_list->ResourceBarrier(1, &blur0_cpdst2read);
    D3D12_RESOURCE_BARRIER blur1_common2ua = CD3DX12_RESOURCE_BARRIER::Transition(p_blur1.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd_list->ResourceBarrier(1, &blur1_common2ua);

    for (int i = 0; i < n_blur; i++) {
        // horizontal blur
        cmd_list->SetPipelineState(hori_pso);

        cmd_list->SetComputeRootDescriptorTable(1, blur0_srv_gpu);
        cmd_list->SetComputeRootDescriptorTable(2, blur1_uav_gpu);
        int n_group_x = (width + 255) / 256;
        cmd_list->Dispatch(n_group_x, height, 1);

        D3D12_RESOURCE_BARRIER blur0_read2ua = CD3DX12_RESOURCE_BARRIER::Transition(p_blur0.Get(),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd_list->ResourceBarrier(1, &blur0_read2ua);
        D3D12_RESOURCE_BARRIER blur1_ua2read = CD3DX12_RESOURCE_BARRIER::Transition(p_blur1.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd_list->ResourceBarrier(1, &blur1_ua2read);

        // vertical blur
        cmd_list->SetPipelineState(vert_pso);

        cmd_list->SetComputeRootDescriptorTable(1, blur1_srv_gpu);
        cmd_list->SetComputeRootDescriptorTable(2, blur0_uav_gpu);
        int n_group_y = (height + 255) / 256;
        cmd_list->Dispatch(width, n_group_y, 1);

        D3D12_RESOURCE_BARRIER blur0_ua2read = CD3DX12_RESOURCE_BARRIER::Transition(p_blur0.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd_list->ResourceBarrier(1, &blur0_ua2read);
        D3D12_RESOURCE_BARRIER blur1_read2ua = CD3DX12_RESOURCE_BARRIER::Transition(p_blur1.Get(),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd_list->ResourceBarrier(1, &blur1_read2ua);
    }
    
    // D3D12_RESOURCE_BARRIER blur0_read2common = CD3DX12_RESOURCE_BARRIER::Transition(p_blur0.Get(),
    //     D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);
    // cmd_list->ResourceBarrier(1, &blur0_read2common);
    D3D12_RESOURCE_BARRIER blur1_ua2common = CD3DX12_RESOURCE_BARRIER::Transition(p_blur1.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    cmd_list->ResourceBarrier(1, &blur1_ua2common);
}

std::vector<float> BlurFilter::GaussWeights(float sigma) const {
    float two_sigma_sqr = 2.0f * sigma * sigma;
    int blur_rad = std::min((int) ceil(2.0f * sigma), kMaxBlurRadius);
    std::vector<float> weights(2 * blur_rad + 1);

    float sum = 0;
    for (int i = -blur_rad; i <= blur_rad; i++) {
        float x = i * i;
        weights[i + blur_rad] = std::exp(-x / two_sigma_sqr);
        sum += weights[i + blur_rad];
    }
    for (float &w : weights) {
        w /= sum;
    }

    return weights;
}

void BlurFilter::BuildDescriptors() {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = fmt;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Format = fmt;
    uav_desc.Texture2D.MipSlice = 0;

    device->CreateShaderResourceView(p_blur0.Get(), &srv_desc, blur0_srv_cpu);
    device->CreateShaderResourceView(p_blur1.Get(), &srv_desc, blur1_srv_cpu);

    device->CreateUnorderedAccessView(p_blur0.Get(), nullptr, &uav_desc, blur0_uav_cpu);
    device->CreateUnorderedAccessView(p_blur1.Get(), nullptr, &uav_desc, blur1_uav_cpu);
}

void BlurFilter::BuildResources() {
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Alignment = 0;
    tex_desc.Width = width;
    tex_desc.Height = height;
    tex_desc.Format = fmt;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    CD3DX12_HEAP_PROPERTIES default_heap_prop(D3D12_HEAP_TYPE_DEFAULT);

    ThrowIfFailed(device->CreateCommittedResource(&default_heap_prop, D3D12_HEAP_FLAG_NONE, &tex_desc,
        // D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p_blur0)));
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p_blur0)));
    ThrowIfFailed(device->CreateCommittedResource(&default_heap_prop, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&p_blur1)));
}