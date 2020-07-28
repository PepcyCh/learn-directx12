#include "RenderTarget.h"

RenderTarget::RenderTarget(ID3D12Device *device, int width, int height, DXGI_FORMAT fmt) :
        device(device), width(width), height(height), fmt(fmt) {
    BuildResource();
}

void RenderTarget::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_srv_cpu,
        CD3DX12_GPU_DESCRIPTOR_HANDLE h_srv_gpu, CD3DX12_CPU_DESCRIPTOR_HANDLE h_rtv_cpu) {
    srv_cpu = h_srv_cpu;
    srv_gpu = h_srv_gpu;
    rtv_cpu = h_rtv_cpu;
    BuildDescriptors();
}

void RenderTarget::OnResize(int new_width, int new_height) {
    if (width != new_height || height != new_height) {
        width = new_width;
        height = new_height;
        BuildResource();
        BuildDescriptors();
    }
}

void RenderTarget::BuildResource() {
    D3D12_RESOURCE_DESC tex_desc = CD3DX12_RESOURCE_DESC::Tex2D(fmt, width, height);
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    CD3DX12_HEAP_PROPERTIES default_heap_prop(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&default_heap_prop, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&p_render_target)));
}

void RenderTarget::BuildDescriptors() {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Format = fmt;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;
    device->CreateShaderResourceView(p_render_target.Get(), &srv_desc, srv_cpu);

    device->CreateRenderTargetView(p_render_target.Get(), nullptr, rtv_cpu);
}