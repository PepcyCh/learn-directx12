#include "SobelFilter.h"
#include "d3dx12.h"

SobelFilter::SobelFilter(ID3D12Device *device, int width, int height, DXGI_FORMAT fmt) :
        device(device), width(width), height(height), fmt(fmt) {
    BuildResources();
}

void SobelFilter::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE h_gpu,
        int inc_size) {
    srv_cpu = h_cpu;
    uav_cpu = h_cpu.Offset(1, inc_size);
    
    srv_gpu = h_gpu;
    uav_gpu = h_gpu.Offset(1, inc_size);

    BuildDescriptors();
}

void SobelFilter::OnResize(int new_width, int new_height) {
    if (new_width != width || new_height != height) {
        width = new_width;
        height = new_height;
        BuildResources();
        BuildDescriptors();
    }
}

void SobelFilter::Execute(ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *pso, CD3DX12_GPU_DESCRIPTOR_HANDLE input) {
    cmd_list->SetPipelineState(pso);
    cmd_list->SetComputeRootSignature(rt_sig);

    cmd_list->SetComputeRootDescriptorTable(0, input);
    cmd_list->SetComputeRootDescriptorTable(2, uav_gpu);

    D3D12_RESOURCE_BARRIER tex_read2ua = CD3DX12_RESOURCE_BARRIER::Transition(p_tex.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd_list->ResourceBarrier(1, &tex_read2ua);

    int n_group_x = (width + 15) / 16;
    int n_group_y = (height + 15) / 16;
    cmd_list->Dispatch(n_group_x, n_group_y, 1);

    D3D12_RESOURCE_BARRIER tex_ua2read = CD3DX12_RESOURCE_BARRIER::Transition(p_tex.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmd_list->ResourceBarrier(1, &tex_ua2read);
}

void SobelFilter::BuildResources() {
    D3D12_RESOURCE_DESC tex_desc = CD3DX12_RESOURCE_DESC::Tex2D(fmt, width, height);
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    CD3DX12_HEAP_PROPERTIES default_heap_prop(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&default_heap_prop, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p_tex)));
}

void SobelFilter::BuildDescriptors() {
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

    device->CreateShaderResourceView(p_tex.Get(), &srv_desc, srv_cpu);
    device->CreateUnorderedAccessView(p_tex.Get(), nullptr, &uav_desc, uav_cpu);
}