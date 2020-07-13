#include "FrameResource.h"

FrameResource::FrameResource(ID3D12Device *device, UINT n_pass, UINT n_obj, UINT n_wave_vertex) {
    ThrowIfFailed(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&p_cmd_alloc)));

    p_pass_cb = std::make_unique<UploadBuffer<PassConst>>(device, n_pass, true);
    p_obj_cb = std::make_unique<UploadBuffer<ObjectConst>>(device, n_obj, true);
    p_wave_vb = std::make_unique<UploadBuffer<Vertex>>(device, n_wave_vertex, false);
}

FrameResource::~FrameResource() {}