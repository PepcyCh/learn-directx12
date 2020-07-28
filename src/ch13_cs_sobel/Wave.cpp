#include "Wave.h"

#include "d3dx12.h"
#include "D3DUtil.h"

using namespace DirectX;

Wave::Wave(ID3D12Device *device, ID3D12GraphicsCommandList *cmd_list, int m, int n, float dx, float dt,
        float speed, float damping) : device(device) {
    n_row = m;
    n_col = n;
    n_vertex = m * n; // should be divisible by 256
    n_triangle = 2 * (m - 1) * (n - 1);
    time_step = dt;
    spatial_step = dx;

    float d = damping * dt + 2.0f;
    float e = (speed * speed) * (dt * dt) / (dx * dx);
    k[0] = (damping * dt - 2.0f) / d;
    k[1] = (4.0f - 8.0f * e) / d;
    k[2] = (2.0f * e) / d;
    
    BuildResources(cmd_list);
}

void Wave::BuildResources(ID3D12GraphicsCommandList *cmd_list) {
    // textures
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Alignment = 0;
    tex_desc.Width = n_col;
    tex_desc.Height = n_row;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R32_FLOAT;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.SampleDesc.Quality = 0;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    CD3DX12_HEAP_PROPERTIES default_heap_prop(D3D12_HEAP_TYPE_DEFAULT);
    ThrowIfFailed(device->CreateCommittedResource(&default_heap_prop, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&prev_solution)));
    ThrowIfFailed(device->CreateCommittedResource(&default_heap_prop, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&curr_solution)));
    ThrowIfFailed(device->CreateCommittedResource(&default_heap_prop, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&next_solution)));

    // upload buffer
    UINT n_subresource = tex_desc.DepthOrArraySize * tex_desc.MipLevels;
    UINT64 upload_buf_size = GetRequiredIntermediateSize(curr_solution.Get(), 0, n_subresource);
    D3D12_RESOURCE_DESC buf_desc = CD3DX12_RESOURCE_DESC::Buffer(upload_buf_size);
    CD3DX12_HEAP_PROPERTIES upload_heap_prop(D3D12_HEAP_TYPE_UPLOAD);
    ThrowIfFailed(device->CreateCommittedResource(&upload_heap_prop, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&prev_uploader)));
    ThrowIfFailed(device->CreateCommittedResource(&upload_heap_prop, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&curr_uploader)));

    // copy initial data & set state to ua
    std::vector<float> init_data(n_col * n_row, 0.0f);
    D3D12_SUBRESOURCE_DATA sub_data = {};
    sub_data.pData = init_data.data();
    sub_data.RowPitch = n_col * sizeof(float);
    sub_data.SlicePitch = n_row * sub_data.RowPitch;

    D3D12_RESOURCE_BARRIER prev_common2copydest = CD3DX12_RESOURCE_BARRIER::Transition(prev_solution.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER prev_copydest2read = CD3DX12_RESOURCE_BARRIER::Transition(prev_solution.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmd_list->ResourceBarrier(1, &prev_common2copydest);
    UpdateSubresources(cmd_list, prev_solution.Get(), prev_uploader.Get(), 0, 0, n_subresource, &sub_data);
    cmd_list->ResourceBarrier(1, &prev_copydest2read);

    D3D12_RESOURCE_BARRIER curr_common2copydest = CD3DX12_RESOURCE_BARRIER::Transition(curr_solution.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER curr_copydest2read = CD3DX12_RESOURCE_BARRIER::Transition(curr_solution.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmd_list->ResourceBarrier(1, &curr_common2copydest);
    UpdateSubresources(cmd_list, curr_solution.Get(), curr_uploader.Get(), 0, 0, n_subresource, &sub_data);
    cmd_list->ResourceBarrier(1, &curr_copydest2read);
    
    D3D12_RESOURCE_BARRIER next_common2ua = CD3DX12_RESOURCE_BARRIER::Transition(next_solution.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd_list->ResourceBarrier(1, &next_common2ua);
}

void Wave::BuildDescriptors(CD3DX12_CPU_DESCRIPTOR_HANDLE h_cpu, CD3DX12_GPU_DESCRIPTOR_HANDLE h_gpu, int inc_size) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;
    srv_desc.Texture2D.MostDetailedMip = 0;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R32_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uav_desc.Texture2D.MipSlice = 0;

    device->CreateShaderResourceView(prev_solution.Get(), &srv_desc, h_cpu);
    device->CreateShaderResourceView(curr_solution.Get(), &srv_desc, h_cpu.Offset(1, inc_size));
    device->CreateShaderResourceView(next_solution.Get(), &srv_desc, h_cpu.Offset(1, inc_size));device->CreateUnorderedAccessView(prev_solution.Get(), nullptr, &uav_desc, h_cpu.Offset(1, inc_size));
    device->CreateUnorderedAccessView(curr_solution.Get(), nullptr, &uav_desc, h_cpu.Offset(1, inc_size));
    device->CreateUnorderedAccessView(next_solution.Get(), nullptr, &uav_desc, h_cpu.Offset(1, inc_size));

    prev_srv = h_gpu;
    curr_srv = h_gpu.Offset(1, inc_size);
    next_srv = h_gpu.Offset(1, inc_size);
    prev_uav = h_gpu.Offset(1, inc_size);
    curr_uav = h_gpu.Offset(1, inc_size);
    next_uav = h_gpu.Offset(1, inc_size);
}

void Wave::Update(const Timer &timer, ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *pso) {
    static float t = 0.0f;
    t += timer.DeltaTime();

    cmd_list->SetPipelineState(pso);
    cmd_list->SetComputeRootSignature(rt_sig);

    if (t >= time_step) {
        cmd_list->SetComputeRoot32BitConstants(0, 3, k, 0);
        cmd_list->SetComputeRootDescriptorTable(1, prev_uav);
        cmd_list->SetComputeRootDescriptorTable(2, curr_uav);
        cmd_list->SetComputeRootDescriptorTable(3, next_uav);

        int n_group_x = (n_col + 15) / 16;
        int n_group_y = (n_row + 15) / 16;
        cmd_list->Dispatch(n_group_x, n_group_y, 1);

        auto temp_solution = prev_solution;
        prev_solution = curr_solution;
        curr_solution = next_solution;
        next_solution = temp_solution;

        auto temp_srv = prev_srv;
        prev_srv = curr_srv;
        curr_srv = next_srv;
        next_srv = temp_srv;

        auto temp_uav = prev_uav;
        prev_uav = curr_uav;
        curr_uav = next_uav;
        next_uav = temp_uav;

        t = 0.0f;

        // curr_solution will be read later in VS
        D3D12_RESOURCE_BARRIER curr_ua2read = CD3DX12_RESOURCE_BARRIER::Transition(curr_solution.Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
        cmd_list->ResourceBarrier(1, &curr_ua2read);
        D3D12_RESOURCE_BARRIER next_read2ua = CD3DX12_RESOURCE_BARRIER::Transition(next_solution.Get(),
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd_list->ResourceBarrier(1, &next_read2ua);
    }
}

void Wave::Disturb(ID3D12GraphicsCommandList *cmd_list, ID3D12RootSignature *rt_sig,
        ID3D12PipelineState *pso, int i, int j, float magnitude) {
    cmd_list->SetPipelineState(pso);
    cmd_list->SetComputeRootSignature(rt_sig);

    int disturb_ind[] = { j, i };
    // cmd_list->SetComputeRoot32BitConstant(0, *reinterpret_cast<int *>(&magnitude), 3);
    cmd_list->SetComputeRoot32BitConstants(0, 1, &magnitude, 3);
    cmd_list->SetComputeRoot32BitConstants(0, 2, disturb_ind, 4);
    cmd_list->SetComputeRootDescriptorTable(3, curr_uav);

    D3D12_RESOURCE_BARRIER read2ua = CD3DX12_RESOURCE_BARRIER::Transition(curr_solution.Get(),
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    cmd_list->ResourceBarrier(1, &read2ua);

    cmd_list->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER ua2read = CD3DX12_RESOURCE_BARRIER::Transition(curr_solution.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ);
    cmd_list->ResourceBarrier(1, &ua2read);
}