#include <array>
#include <algorithm>

#include <d3dcompiler.h>
#include <DirectXColors.h>

#include "ResourceUploadBatch.h"
#include "DDSTextureLoader.h"

#include "../defines.h"
#include "D3DApp.h"
#include "D3DUtil.h"
#include "GeometryGenerator.h"
#include "FrameResource.h"
#include "Wave.h"
#include "BlurFilter.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

const int n_frame_resource = 3;

struct RenderItem {
    XMFLOAT4X4 model = DXMath::Identity4x4();
    XMFLOAT4X4 tex_transform = DXMath::Identity4x4(); // uv = mat_trans * tex_trans * (uv, 0, 1)
    int n_frame_dirty = n_frame_resource;
    UINT obj_cb_ind = -1;
    MeshGeometry *geo = nullptr;
    Material *mat = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT n_index = 0;
    UINT start_index = 0;
    int base_vertex = 0;
    XMFLOAT2 displacement_map_texel_size = { 0.0f, 0.0f };
    float grid_spatial_step = 0.0f;
};

enum class RenderLayor : size_t {
    Opaque,      // opaque models
    AlphaTested, // models with transparent part
    Transparent, // transparent models that need blending
    GpuWave,     // wave updated via CS
    Count
};

// do blur by copying render-target to resource in BlurFilter and then copying back

class D3DAppBlur : public D3DApp {
  public:
    D3DAppBlur(HINSTANCE h_inst) : D3DApp(h_inst) {}
    ~D3DAppBlur() {
        if (p_device != nullptr) {
            FlushCommandQueue();
        }
    }

    bool Initialize() override {
        if (!D3DApp::Initialize()) {
            return false;
        }

        ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), nullptr));

        p_wave = std::make_unique<Wave>(p_device.Get(), p_cmd_list.Get(), 128, 128, 1.0f, 0.03f, 4.0f, 0.2f);
        p_blur_filter = std::make_unique<BlurFilter>(p_device.Get(), client_width, client_height, back_buffer_fmt);

        LoadTextures();
        BuildRootSignature();
        BuildWaveRootSignature();
        BuildPostRootSignature();
        BuildDescriptorHeaps();
        BuildShaderAndInputLayout();
        BuildLandGeometry();
        BuildWaveGeometry();
        BuildBoxGeometry();
        BuildMaterials();
        BuildRenderItems();
        BuildFrameResources();
        BuildPSOs();

        ThrowIfFailed(p_cmd_list->Close());
        ID3D12CommandList *cmds[] = { p_cmd_list.Get() };
        p_cmd_queue->ExecuteCommandLists(sizeof(cmds) / sizeof(cmds[0]), cmds);
        FlushCommandQueue();

        return true;
    }

  private:
    void OnResize() override {
        D3DApp::OnResize();
        XMMATRIX _proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, Aspect(), 0.1f, 1000.0f);
        XMStoreFloat4x4(&proj, _proj);
        if (p_blur_filter) {
            p_blur_filter->OnResize(client_width, client_height);
        }
    }
    void Update(const Timer &timer) override {
        OnKeyboardInput(timer);
        UpdateCamera(timer);

        // change current frame resource and wait
        curr_fr_ind = (curr_fr_ind + 1) % n_frame_resource;
        curr_fr = frame_resources[curr_fr_ind].get();
        if (curr_fr->fence != 0 && p_fence->GetCompletedValue() < curr_fr->fence) {
            HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
            ThrowIfFailed(p_fence->SetEventOnCompletion(curr_fr->fence, event));
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }

        AnimateMaterials(timer);
        UpdateObjectCB(timer);
        UpdatePassCB(timer);
        UpdateMaterialCB(timer);
    }
    void Draw(const Timer &timer) override {
        // reset cmd list and cmd alloc
        auto cmd_alloc = curr_fr->p_cmd_alloc;
        ThrowIfFailed(cmd_alloc->Reset());
        ThrowIfFailed(p_cmd_list->Reset(cmd_alloc.Get(), psos["opaque"].Get()));

        // viewport and scissor
        p_cmd_list->RSSetViewports(1, &viewport);
        p_cmd_list->RSSetScissorRects(1, &scissors);

        // back buffer: present -> render target
        auto present2rt = CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        p_cmd_list->ResourceBarrier(1, &present2rt);
        // clear rtv and dsv
        auto back_buffer_view = CurrBackBufferView();
        auto depth_stencil_view = DepthStencilView();
        p_cmd_list->ClearRenderTargetView(back_buffer_view, (float *) &main_pass_cb.fog_color, 0, nullptr);
        p_cmd_list->ClearDepthStencilView(depth_stencil_view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f, 0, 0, nullptr);
        // set rtv and dsv
        p_cmd_list->OMSetRenderTargets(1, &back_buffer_view, true, &depth_stencil_view);
        
        // set cbv/srv/uav heaps
        ID3D12DescriptorHeap *heaps[] = { p_cbv_srv_uav_heap.Get() };
        p_cmd_list->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);
        // update wave
        UpdateWaves(timer);

        // set root signature
        p_cmd_list->SetGraphicsRootSignature(p_rt_sig.Get());
        // set per pass cbv
        auto pass_cb = curr_fr->p_pass_cb->Resource();
        p_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());
        auto displacement_map = p_wave->DisplacementMap();
        p_cmd_list->SetGraphicsRootDescriptorTable(4, displacement_map);

        // draw opaque items
        p_cmd_list->SetPipelineState(psos["opaque"].Get());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::Opaque]);
        // draw alpha tested items
        p_cmd_list->SetPipelineState(psos["alpha_tested"].Get());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::AlphaTested]);
        // draw transparent items
        p_cmd_list->SetPipelineState(psos["transparent"].Get());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::Transparent]);
        // draw GPU wave
        p_cmd_list->SetPipelineState(psos["gpu_wave"].Get());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::GpuWave]);

        // post process
        p_blur_filter->Execute(p_cmd_list.Get(), p_post_rt_sig.Get(), psos["blur_hori"].Get(),
            psos["blur_vert"].Get(), CurrBackBuffer(), 4);
        D3D12_RESOURCE_BARRIER cpsrc2cpdst = CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        p_cmd_list->ResourceBarrier(1, &cpsrc2cpdst);
        p_cmd_list->CopyResource(CurrBackBuffer(), p_blur_filter->Output());
        D3D12_RESOURCE_BARRIER cpdst2present = CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        p_cmd_list->ResourceBarrier(1, &cpdst2present);
        
        // close & begin execution
        ThrowIfFailed(p_cmd_list->Close());
        ID3D12CommandList *cmds[] = { p_cmd_list.Get() };
        p_cmd_queue->ExecuteCommandLists(sizeof(cmds) / sizeof(cmds[0]), cmds);

        // swap
        p_swap_chain->Present(0, 0);
        curr_back_buffer = (curr_back_buffer + 1) % kSwapChainBufferCnt;

        // fence for current frame resource
        curr_fr->fence = ++curr_fence;
        p_cmd_queue->Signal(p_fence.Get(), curr_fr->fence);
    }

    void OnMouseDown(WPARAM btn_state, int x, int y) override {
        last_mouse.x = x;
        last_mouse.y = y;

        SetCapture(h_win);
    }
    void OnMouseMove(WPARAM btn_state, int x, int y) override {
        if (btn_state & MK_LBUTTON) {
            float dx = XMConvertToRadians(0.25f * (x - last_mouse.x));
            float dy = XMConvertToRadians(0.25f * (y - last_mouse.y));
            theta -= dx;
            phi += dy;
            phi = std::clamp(phi, 0.1f, XM_PI - 0.1f);
        } if (btn_state & MK_RBUTTON) {
            float dx = 0.05f * (x - last_mouse.x);
            float dy = 0.05f * (y - last_mouse.y);
            radius += dx - dy;
            radius = std::clamp(radius, 5.0f, 150.0f);
        }
        last_mouse.x = x;
        last_mouse.y = y;
    }
    void OnMouseUp(WPARAM btn_state, int x, int y) override {
        ReleaseCapture();
    }
    void OnKeyboardInput(const Timer &timer) {
        ;
    }

    void UpdateCamera(const Timer &timer) {
        float x = radius * std::sin(phi) * std::cos(theta);
        float y = radius * std::cos(phi);
        float z = radius * std::sin(phi) * std::sin(theta);
        eye = { x, y, z };

        XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
        XMVECTOR lookat = XMVectorZero();
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMMATRIX _view = XMMatrixLookAtRH(pos, lookat, up);
        XMStoreFloat4x4(&view, _view);
    }
    void UpdateObjectCB(const Timer &timer) {
        auto curr_obj_cb = curr_fr->p_obj_cb.get();
        for (auto &item : items) {
            if (item->n_frame_dirty > 0) {
                ObjectConst obj_const;
                XMMATRIX model = XMLoadFloat4x4(&item->model);
                XMStoreFloat4x4(&obj_const.model, model);
                XMMATRIX model_t = XMMatrixTranspose(model);
                auto model_t_det = XMMatrixDeterminant(model_t);
                XMMATRIX model_it = XMMatrixInverse(&model_t_det, model_t);
                XMStoreFloat4x4(&obj_const.model_it, model_it);
                XMMATRIX tex_transform = XMLoadFloat4x4(&item->tex_transform);
                XMStoreFloat4x4(&obj_const.tex_transform, tex_transform);
                obj_const.displacement_map_texel_size = item->displacement_map_texel_size;
                obj_const.grid_spatial_step = item->grid_spatial_step;
                curr_obj_cb->CopyData(item->obj_cb_ind, obj_const);
                --item->n_frame_dirty;
            }
        }
    }
    void UpdatePassCB(const Timer &timer) {
        XMMATRIX _view = XMLoadFloat4x4(&view);
        XMMATRIX _proj = XMLoadFloat4x4(&proj);

        auto _view_det = XMMatrixDeterminant(_view);
        XMMATRIX _view_inv = XMMatrixInverse(&_view_det, _view);
        auto _proj_det = XMMatrixDeterminant(_proj);
        XMMATRIX _proj_inv = XMMatrixInverse(&_proj_det, _proj);
        XMMATRIX _vp = XMMatrixMultiply(_view, _proj);
        auto _vp_det = XMMatrixDeterminant(_vp);
        XMMATRIX _vp_inv = XMMatrixInverse(&_vp_det, _vp);

        XMStoreFloat4x4(&main_pass_cb.view, _view);
        XMStoreFloat4x4(&main_pass_cb.view_inv, _view_inv);
        XMStoreFloat4x4(&main_pass_cb.proj, _proj);
        XMStoreFloat4x4(&main_pass_cb.proj_inv, _proj_inv);
        XMStoreFloat4x4(&main_pass_cb.vp, _vp);
        XMStoreFloat4x4(&main_pass_cb.vp_inv, _vp_inv);
        main_pass_cb.eye = eye;
        main_pass_cb.rt_size = { (float) client_width, (float) client_height };
        main_pass_cb.rt_size_inv = { 1.0f / client_width, 1.0f / client_height };
        main_pass_cb.near_z = 0.1f;
        main_pass_cb.far_z = 1000.0f;
        main_pass_cb.delta_time = timer.DeltaTime();
        main_pass_cb.total_time = timer.TotalTime();
        main_pass_cb.ambient = { 0.25f, 0.25f, 0.35f, 1.0f };
        main_pass_cb.fog_color = { 0.7f, 0.7f, 0.7f, 1.0f };
        main_pass_cb.fog_start = 25.0f;
        main_pass_cb.fog_end = 150.0f;
        main_pass_cb.lights[0].direction = { 0.57735f, -0.57735f, 0.57735f };
        main_pass_cb.lights[0].strength = { 0.6f, 0.6f, 0.6f };
        main_pass_cb.lights[1].direction = { -0.57735f, -0.57735f, 0.57735f };
        main_pass_cb.lights[1].strength = { 0.3f, 0.3f, 0.3f };
        main_pass_cb.lights[2].direction = { 0.0f, -0.707f, -0.707f };
        main_pass_cb.lights[2].strength = { 0.15f, 0.15f, 0.15f };

        auto curr_pass_cb = curr_fr->p_pass_cb.get();
        curr_pass_cb->CopyData(0, main_pass_cb);
    }
    void UpdateMaterialCB(const Timer &timer) {
        auto curr_mat_cb = curr_fr->p_mat_cb.get();
        for (auto &[_, _mat] : materials) {
            Material *mat = _mat.get();
            if (mat->n_frame_dirty > 0) {
                XMMATRIX mat_transform = XMLoadFloat4x4(&mat->mat_transform);
                MaterialConst mat_const;
                mat_const.albedo = mat->albedo;
                mat_const.fresnel_r0 = mat->fresnel_r0;
                mat_const.roughness = mat->roughness;
                XMStoreFloat4x4(&mat_const.mat_transform, mat_transform);
                curr_mat_cb->CopyData(mat->mat_cb_ind, mat_const);
                --mat->n_frame_dirty;
            }
        }
    }
    void UpdateWaves(const Timer &timer) {
        // Every quarter second, generate a random wave.
        static float t_base = 0.0f;
        if ((this->timer.TotalTime() - t_base) >= 0.25f) {
            t_base += 0.25f;

            int i = DXMath::RandI(4, p_wave->RowCount() - 5);
            int j = DXMath::RandI(4, p_wave->ColumnCount() - 5);

            float r = DXMath::RandF(0.2f, 0.5f);

            p_wave->Disturb(p_cmd_list.Get(), p_wave_rt_sig.Get(), psos["wave_disturb"].Get(), i, j, r);
        }

        // Update the wave simulation.
        p_wave->Update(timer, p_cmd_list.Get(), p_wave_rt_sig.Get(), psos["wave_update"].Get());
    }
    void AnimateMaterials(const Timer &timer) {
        auto water_mat = materials["water"].get();
        float &u = water_mat->mat_transform(3, 0);
        float &v = water_mat->mat_transform(3, 1);
        u += 0.1f * timer.DeltaTime();
        v += 0.02f * timer.DeltaTime();
        if (u >= 1.0f) {
            u -= 1.0f;
        }
        if (v >= 1.0f) {
            v -= 1.0f;
        }
        water_mat->n_frame_dirty = n_frame_resource;
    }

    std::array<const CD3DX12_STATIC_SAMPLER_DESC, 6> GetStaticSampler() {
        const CD3DX12_STATIC_SAMPLER_DESC point_wrap(0, D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        const CD3DX12_STATIC_SAMPLER_DESC point_clamp(1, D3D12_FILTER_MIN_MAG_MIP_POINT,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        const CD3DX12_STATIC_SAMPLER_DESC linear_wrap(2, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        const CD3DX12_STATIC_SAMPLER_DESC linear_clamp(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        const CD3DX12_STATIC_SAMPLER_DESC aniso_wrap(4, D3D12_FILTER_ANISOTROPIC,
            D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
        const CD3DX12_STATIC_SAMPLER_DESC aniso_clamp(5, D3D12_FILTER_ANISOTROPIC,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
        return { point_wrap, point_clamp, linear_wrap, linear_clamp, aniso_wrap, aniso_clamp };
    }
    void LoadTextures() {
        // use DirectXTK12, different from d3d12book

        // prepare upload buffer
        ResourceUploadBatch resource_upload(p_device.Get());
        resource_upload.Begin();

        auto grass_tex = std::make_unique<Texture>();
        grass_tex->name = "grass";
        grass_tex->filename = root_path + L"textures/grass.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            grass_tex->filename.c_str(), &grass_tex->resource));
        textures[grass_tex->name] = std::move(grass_tex);

        auto water_tex = std::make_unique<Texture>();
        water_tex->name = "water";
        water_tex->filename = root_path + L"textures/water1.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            water_tex->filename.c_str(), &water_tex->resource));
        textures[water_tex->name] = std::move(water_tex);

        auto fence_tex = std::make_unique<Texture>();
        fence_tex->name = "fence";
        fence_tex->filename = root_path + L"textures/WireFence.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            fence_tex->filename.c_str(), &fence_tex->resource));
        textures[fence_tex->name] = std::move(fence_tex);

        // do copy & flush
        // upload_finish is std::future<void>
        auto upload_finish = resource_upload.End(p_cmd_queue.Get());
        FlushCommandQueue();
        upload_finish.wait();
    }

    void BuildRootSignature() {
        // for SRV and UAV, only buffer resources where the SRV/UAV format contains only
        // 32 bit FLOAT/UINT/SINT components (there is no format conversion) can be used
        // as root descriptors

        CD3DX12_ROOT_PARAMETER rt_params[5];
        rt_params[0].InitAsConstantBufferView(0);
        rt_params[1].InitAsConstantBufferView(1);
        rt_params[2].InitAsConstantBufferView(2);
        CD3DX12_DESCRIPTOR_RANGE srv_range0(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        rt_params[3].InitAsDescriptorTable(1, &srv_range0, D3D12_SHADER_VISIBILITY_PIXEL);
        CD3DX12_DESCRIPTOR_RANGE srv_range1(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
        rt_params[4].InitAsDescriptorTable(1, &srv_range1, D3D12_SHADER_VISIBILITY_VERTEX);

        auto static_samplers = GetStaticSampler();
        CD3DX12_ROOT_SIGNATURE_DESC rt_sig_desc(sizeof(rt_params) / sizeof(rt_params[0]), rt_params,
            static_samplers.size(), static_samplers.data(),
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> serialized_rt_sig = nullptr;
        ComPtr<ID3DBlob> error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rt_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized_rt_sig, &error);
        if (error != nullptr) {
            OutputDebugStringA((char *) error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        ThrowIfFailed(p_device->CreateRootSignature(0, serialized_rt_sig->GetBufferPointer(),
            serialized_rt_sig->GetBufferSize(), IID_PPV_ARGS(&p_rt_sig)));
    }
    void BuildWaveRootSignature() {
        CD3DX12_ROOT_PARAMETER rt_params[4];
        rt_params[0].InitAsConstants(6, 0);
        CD3DX12_DESCRIPTOR_RANGE uav_range0(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        rt_params[1].InitAsDescriptorTable(1, &uav_range0);
        CD3DX12_DESCRIPTOR_RANGE uav_range1(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1);
        rt_params[2].InitAsDescriptorTable(1, &uav_range1);
        CD3DX12_DESCRIPTOR_RANGE uav_range2(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 2);
        rt_params[3].InitAsDescriptorTable(1, &uav_range2);

        CD3DX12_ROOT_SIGNATURE_DESC rt_sig_desc(sizeof(rt_params) / sizeof(rt_params[0]), rt_params,
            0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> serialized_rt_sig = nullptr;
        ComPtr<ID3DBlob> error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rt_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized_rt_sig, &error);
        if (error != nullptr) {
            OutputDebugStringA((char *) error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        ThrowIfFailed(p_device->CreateRootSignature(0, serialized_rt_sig->GetBufferPointer(),
            serialized_rt_sig->GetBufferSize(), IID_PPV_ARGS(&p_wave_rt_sig)));
    }
    void BuildPostRootSignature() {
        CD3DX12_ROOT_PARAMETER rt_params[3];
        rt_params[0].InitAsConstants(12, 0);
        CD3DX12_DESCRIPTOR_RANGE srv_range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        rt_params[1].InitAsDescriptorTable(1, &srv_range);
        CD3DX12_DESCRIPTOR_RANGE uav_range(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0);
        rt_params[2].InitAsDescriptorTable(1, &uav_range);

        CD3DX12_ROOT_SIGNATURE_DESC rt_sig_desc(sizeof(rt_params) / sizeof(rt_params[0]), rt_params,
            0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

        ComPtr<ID3DBlob> serialized_rt_sig = nullptr;
        ComPtr<ID3DBlob> error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rt_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized_rt_sig, &error);
        if (error != nullptr) {
            OutputDebugStringA((char *) error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        ThrowIfFailed(p_device->CreateRootSignature(0, serialized_rt_sig->GetBufferPointer(),
            serialized_rt_sig->GetBufferSize(), IID_PPV_ARGS(&p_post_rt_sig)));
    }
    void BuildDescriptorHeaps() {
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
        srv_heap_desc.NumDescriptors = textures.size() + p_wave->DescriptorCount() +
            p_blur_filter->DescriptorCount();
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(p_device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&p_cbv_srv_uav_heap)));

        CD3DX12_CPU_DESCRIPTOR_HANDLE h_srv(p_cbv_srv_uav_heap->GetCPUDescriptorHandleForHeapStart());
        auto grass_tex = textures["grass"]->resource;
        auto water_tex = textures["water"]->resource;
        auto fence_tex = textures["fence"]->resource;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = grass_tex->GetDesc().Format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = -1;
        p_device->CreateShaderResourceView(grass_tex.Get(), &srv_desc, h_srv);

        h_srv.Offset(1, cbv_srv_uav_descriptor_size);
        srv_desc.Format = water_tex->GetDesc().Format;
        p_device->CreateShaderResourceView(water_tex.Get(), &srv_desc, h_srv);

        h_srv.Offset(1, cbv_srv_uav_descriptor_size);
        srv_desc.Format = fence_tex->GetDesc().Format;
        p_device->CreateShaderResourceView(fence_tex.Get(), &srv_desc, h_srv);

        p_wave->BuildDescriptors(
            CD3DX12_CPU_DESCRIPTOR_HANDLE(p_cbv_srv_uav_heap->GetCPUDescriptorHandleForHeapStart(),
                textures.size(), cbv_srv_uav_descriptor_size),
            CD3DX12_GPU_DESCRIPTOR_HANDLE(p_cbv_srv_uav_heap->GetGPUDescriptorHandleForHeapStart(),
                textures.size(), cbv_srv_uav_descriptor_size),
            cbv_srv_uav_descriptor_size);

        p_blur_filter->BuildDescriptors(
            CD3DX12_CPU_DESCRIPTOR_HANDLE(p_cbv_srv_uav_heap->GetCPUDescriptorHandleForHeapStart(),
                textures.size() + p_wave->DescriptorCount(), cbv_srv_uav_descriptor_size),
            CD3DX12_GPU_DESCRIPTOR_HANDLE(p_cbv_srv_uav_heap->GetGPUDescriptorHandleForHeapStart(),
                textures.size() + p_wave->DescriptorCount(), cbv_srv_uav_descriptor_size),
            cbv_srv_uav_descriptor_size);
    }
    void BuildShaderAndInputLayout() {
        // defines
        const D3D_SHADER_MACRO fog_defines[] = {
            "FOG", "1",
            nullptr, nullptr
        };
        const D3D_SHADER_MACRO alpha_test_defines[] = {
            "FOG", "1",
            "ALPHA_TEST", "1",
            nullptr, nullptr
        };
        const D3D_SHADER_MACRO displacement_defines[] = {
            "DISPLACEMENT_MAP", "1",
            nullptr, nullptr
        };

        // build shader from hlsl file
        shaders["standard_vs"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/P3N3U2_default.hlsl",
            nullptr, "VS", "vs_5_1");
        shaders["displacement_vs"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/P3N3U2_default.hlsl",
            displacement_defines, "VS", "vs_5_1");
        shaders["opaque_ps"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/P3N3U2_default.hlsl",
            fog_defines, "PS", "ps_5_1");
        shaders["alpha_tested_ps"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/P3N3U2_default.hlsl",
            alpha_test_defines, "PS", "ps_5_1");
        shaders["wave_update_cs"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/wave.hlsl",
            nullptr, "UpdateWaveCS", "cs_5_1");
        shaders["wave_disturb_cs"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/wave.hlsl",
            nullptr, "DisturbCS", "cs_5_1");
        shaders["blur_hori_cs"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/blur.hlsl",
            nullptr, "HoriBlurCS", "cs_5_1");
        shaders["blur_vert_cs"] = D3DUtil::CompileShader(src_path + L"ch13_cs_blur/shaders/blur.hlsl",
            nullptr, "VertBlurCS", "cs_5_1");

        // input layout and input elements specify input of (vertex) shader
        input_layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
    }
    void BuildLandGeometry() {
        GeometryGenerator geo_gen;
        GeometryGenerator::MeshData grid = geo_gen.Grid(160.0f, 160.0f, 50, 50);

        std::vector<Vertex> vertices(grid.vertices.size());
        for (int i = 0; i < vertices.size(); i++) {
            const auto &p = grid.vertices[i].pos;
            vertices[i].pos = p;
            vertices[i].pos.y = GetHillHeight(p.x, p.z);
            vertices[i].norm = GetHillNormal(p.x, p.z);
            vertices[i].texc = grid.vertices[i].texc;
        }
        std::vector<uint16_t> indices = grid.GetIndices16();

        const UINT vb_size = vertices.size() * sizeof(Vertex);
        const UINT ib_size = indices.size() * sizeof(uint16_t);

        auto geo = std::make_unique<MeshGeometry>();
        geo->name = "land_geo";

        ThrowIfFailed(D3DCreateBlob(vb_size, &geo->vb_cpu));
        CopyMemory(geo->vb_cpu->GetBufferPointer(), vertices.data(), vb_size);
        ThrowIfFailed(D3DCreateBlob(ib_size, &geo->ib_cpu));
        CopyMemory(geo->ib_cpu->GetBufferPointer(), indices.data(), ib_size);

        geo->vb_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            vertices.data(), vb_size, geo->vb_uploader);
        geo->ib_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            indices.data(), ib_size, geo->ib_uploader);

        geo->vb_stride = sizeof(Vertex);
        geo->vb_size = vb_size;
        geo->index_fmt = DXGI_FORMAT_R16_UINT;
        geo->ib_size = ib_size;

        SubmeshGeometry submesh;
        submesh.n_index = indices.size();
        submesh.start_index = 0;
        submesh.base_vertex = 0;
        geo->draw_args["grid"] = submesh;

        geometries[geo->name] = std::move(geo);
    }
    void BuildWaveGeometry() {
        GeometryGenerator geo_gen;
        GeometryGenerator::MeshData grid = geo_gen.Grid(160.0f, 160.0f, p_wave->RowCount(), p_wave->ColumnCount());

        std::vector<Vertex> vertices(grid.vertices.size());
        for (int i = 0; i < vertices.size(); i++) {
            vertices[i].pos = grid.vertices[i].pos;
            vertices[i].norm = grid.vertices[i].norm;
            vertices[i].texc = grid.vertices[i].texc;
        }
        std::vector<uint16_t> indices = grid.GetIndices16();

        const UINT vb_size = vertices.size() * sizeof(Vertex);
        const UINT ib_size = indices.size() * sizeof(uint16_t);

        auto geo = std::make_unique<MeshGeometry>();
        geo->name = "water_geo";

        ThrowIfFailed(D3DCreateBlob(vb_size, &geo->vb_cpu));
        CopyMemory(geo->vb_cpu->GetBufferPointer(), vertices.data(), vb_size);
        ThrowIfFailed(D3DCreateBlob(ib_size, &geo->ib_cpu));
        CopyMemory(geo->ib_cpu->GetBufferPointer(), indices.data(), ib_size);

        geo->vb_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            vertices.data(), vb_size, geo->vb_uploader);
        geo->ib_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            indices.data(), ib_size, geo->ib_uploader);

        geo->vb_stride = sizeof(Vertex);
        geo->vb_size = vb_size;
        geo->index_fmt = DXGI_FORMAT_R16_UINT;
        geo->ib_size = ib_size;

        SubmeshGeometry submesh;
        submesh.n_index = indices.size();
        submesh.start_index = 0;
        submesh.base_vertex = 0;
        geo->draw_args["grid"] = submesh;

        geometries[geo->name] = std::move(geo);
    }
    void BuildBoxGeometry() {
        GeometryGenerator geo_gen;
        GeometryGenerator::MeshData box = geo_gen.Box(8.0f, 8.0f, 8.0f, 3);

        std::vector<Vertex> vertices(box.vertices.size());
        for (int i = 0; i < vertices.size(); i++) {
            vertices[i].pos = box.vertices[i].pos;
            vertices[i].norm = box.vertices[i].norm;
            vertices[i].texc = box.vertices[i].texc;
        }
        std::vector<uint16_t> indices = box.GetIndices16();

        UINT vb_size = vertices.size() * sizeof(Vertex);
        UINT ib_size = indices.size() * sizeof(uint16_t);

        auto geo = std::make_unique<MeshGeometry>();
        geo->name = "box_geo";

        ThrowIfFailed(D3DCreateBlob(vb_size, &geo->vb_cpu));
        CopyMemory(geo->vb_cpu->GetBufferPointer(), vertices.data(), vb_size);
        ThrowIfFailed(D3DCreateBlob(ib_size, &geo->ib_cpu));
        CopyMemory(geo->ib_cpu->GetBufferPointer(), indices.data(), ib_size);

        geo->vb_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            vertices.data(), vb_size, geo->vb_uploader);
        geo->ib_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            indices.data(), ib_size, geo->ib_uploader);

        geo->vb_stride = sizeof(Vertex);
        geo->vb_size = vb_size;
        geo->index_fmt = DXGI_FORMAT_R16_UINT;
        geo->ib_size = ib_size;

        SubmeshGeometry submesh;
        submesh.n_index = indices.size();
        submesh.start_index = 0;
        submesh.base_vertex = 0;
        geo->draw_args["box"] = submesh;

        geometries[geo->name] = std::move(geo);
    }
    void BuildMaterials() {
        auto grass = std::make_unique<Material>();
        grass->name = "grass";
        grass->n_frame_dirty = n_frame_resource;
        grass->mat_cb_ind = 0;
        grass->diffuse_srv_heap_index = 0;
        grass->albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        grass->fresnel_r0 = { 0.01f, 0.01f, 0.01f };
        grass->roughness = 0.125f;
        materials[grass->name] = std::move(grass);

        auto water = std::make_unique<Material>();
        water->name = "water";
        water->n_frame_dirty = n_frame_resource;
        water->mat_cb_ind = 1;
        water->diffuse_srv_heap_index = 1;
        water->albedo = { 1.0f, 1.0f, 1.0f, 0.5f }; // transparent water
        water->fresnel_r0 = { 0.2f, 0.2f, 0.2f };
        water->roughness = 0.0f;
        materials[water->name] = std::move(water);

        auto fence = std::make_unique<Material>();
        fence->name = "fence";
        fence->n_frame_dirty = n_frame_resource;
        fence->mat_cb_ind = 2;
        fence->diffuse_srv_heap_index = 2;
        fence->albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        fence->fresnel_r0 = { 0.1f, 0.1f, 0.1f };
        fence->roughness = 0.25f;
        materials[fence->name] = std::move(fence);
    }
    void BuildPSOs() {
        // graphics pso
        D3D12_GRAPHICS_PIPELINE_STATE_DESC opaque_pso_desc = {};
        opaque_pso_desc.InputLayout = { input_layout.data(), (UINT) input_layout.size() };
        opaque_pso_desc.pRootSignature = p_rt_sig.Get();
        opaque_pso_desc.VS = {
            reinterpret_cast<BYTE *>(shaders["standard_vs"]->GetBufferPointer()),
            shaders["standard_vs"]->GetBufferSize()
        };
        opaque_pso_desc.PS = {
            reinterpret_cast<BYTE *>(shaders["opaque_ps"]->GetBufferPointer()),
            shaders["opaque_ps"]->GetBufferSize()
        };
        opaque_pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        opaque_pso_desc.RasterizerState.FrontCounterClockwise = true; // ccw front face
        opaque_pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        opaque_pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        opaque_pso_desc.SampleMask = UINT_MAX;
        opaque_pso_desc.SampleDesc.Count = 1;
        opaque_pso_desc.SampleDesc.Quality = 0;
        opaque_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        opaque_pso_desc.NumRenderTargets = 1;
        opaque_pso_desc.RTVFormats[0] = back_buffer_fmt;
        opaque_pso_desc.DSVFormat = depth_stencil_fmt;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&opaque_pso_desc, IID_PPV_ARGS(&psos["opaque"])));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC transparent_pso_desc = opaque_pso_desc;
        D3D12_RENDER_TARGET_BLEND_DESC transparent_blend_desc = {};
        transparent_blend_desc.BlendEnable = true;
        transparent_blend_desc.LogicOpEnable = false;
        transparent_blend_desc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        transparent_blend_desc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        transparent_blend_desc.BlendOp = D3D12_BLEND_OP_ADD;
        transparent_blend_desc.SrcBlendAlpha = D3D12_BLEND_ONE;
        transparent_blend_desc.DestBlendAlpha = D3D12_BLEND_ZERO;
        transparent_blend_desc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        transparent_blend_desc.LogicOp = D3D12_LOGIC_OP_NOOP;
        transparent_blend_desc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        transparent_pso_desc.BlendState.RenderTarget[0] = transparent_blend_desc;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&transparent_pso_desc,
            IID_PPV_ARGS(&psos["transparent"])));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC alpha_tested_pso_desc = opaque_pso_desc;
        alpha_tested_pso_desc.PS = {
            reinterpret_cast<BYTE * >(shaders["alpha_tested_ps"]->GetBufferPointer()),
            shaders["alpha_tested_ps"]->GetBufferSize()
        };
        alpha_tested_pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&alpha_tested_pso_desc,
            IID_PPV_ARGS(&psos["alpha_tested"])));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC gpu_wave_pso_desc = transparent_pso_desc;
        gpu_wave_pso_desc.VS = {
            reinterpret_cast<BYTE *>(shaders["displacement_vs"]->GetBufferPointer()),
            shaders["displacement_vs"]->GetBufferSize()
        };
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&gpu_wave_pso_desc,
            IID_PPV_ARGS(&psos["gpu_wave"])));

        // compute pso
        D3D12_COMPUTE_PIPELINE_STATE_DESC wave_update_pso_desc = {};
        wave_update_pso_desc.CS = {
            reinterpret_cast<BYTE *>(shaders["wave_update_cs"]->GetBufferPointer()),
            shaders["wave_update_cs"]->GetBufferSize()
        };
        wave_update_pso_desc.pRootSignature = p_wave_rt_sig.Get();
        wave_update_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ThrowIfFailed(p_device->CreateComputePipelineState(&wave_update_pso_desc,
            IID_PPV_ARGS(&psos["wave_update"])));

        D3D12_COMPUTE_PIPELINE_STATE_DESC wave_disturb_pso_desc = wave_update_pso_desc;
        wave_disturb_pso_desc.CS = {
            reinterpret_cast<BYTE *>(shaders["wave_disturb_cs"]->GetBufferPointer()),
            shaders["wave_disturb_cs"]->GetBufferSize()
        };
        ThrowIfFailed(p_device->CreateComputePipelineState(&wave_disturb_pso_desc,
            IID_PPV_ARGS(&psos["wave_disturb"])));

        D3D12_COMPUTE_PIPELINE_STATE_DESC blur_hori_pso_desc = {};
        blur_hori_pso_desc.CS = {
            reinterpret_cast<BYTE *>(shaders["blur_hori_cs"]->GetBufferPointer()),
            shaders["blur_hori_cs"]->GetBufferSize()
        };
        blur_hori_pso_desc.pRootSignature = p_post_rt_sig.Get();
        blur_hori_pso_desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        ThrowIfFailed(p_device->CreateComputePipelineState(&blur_hori_pso_desc,
            IID_PPV_ARGS(&psos["blur_hori"])));

        D3D12_COMPUTE_PIPELINE_STATE_DESC blur_vert_pso_desc = blur_hori_pso_desc;
        blur_vert_pso_desc.CS = {
            reinterpret_cast<BYTE *>(shaders["blur_vert_cs"]->GetBufferPointer()),
            shaders["blur_vert_cs"]->GetBufferSize()
        };
        ThrowIfFailed(p_device->CreateComputePipelineState(&blur_vert_pso_desc,
            IID_PPV_ARGS(&psos["blur_vert"])));
    }
    void BuildFrameResources() {
        for (int i = 0; i < n_frame_resource; i++) {
            frame_resources.push_back(std::make_unique<FrameResource>(p_device.Get(), 1,
                items.size(), materials.size()));
        }
    }
    void BuildRenderItems() {
        int obj_cb_ind = 0;

        auto grid_ritem = std::make_unique<RenderItem>();
        grid_ritem->model = DXMath::Identity4x4();
        XMStoreFloat4x4(&grid_ritem->tex_transform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
        grid_ritem->obj_cb_ind = obj_cb_ind++;
        grid_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        grid_ritem->mat = materials["grass"].get();
        grid_ritem->geo = geometries["land_geo"].get();
        grid_ritem->n_index = grid_ritem->geo->draw_args["grid"].n_index;
        grid_ritem->start_index = grid_ritem->geo->draw_args["grid"].start_index;
        grid_ritem->base_vertex = grid_ritem->geo->draw_args["grid"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Opaque].push_back(grid_ritem.get());
        items.push_back(std::move(grid_ritem));

        auto wave_ritem = std::make_unique<RenderItem>();
        wave_ritem->model = DXMath::Identity4x4();
        XMStoreFloat4x4(&wave_ritem->tex_transform, XMMatrixScaling(5.0f, 5.0f, 1.0f));
        wave_ritem->obj_cb_ind = obj_cb_ind++;
        wave_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        wave_ritem->mat = materials["water"].get();
        wave_ritem->geo = geometries["water_geo"].get();
        wave_ritem->n_index = wave_ritem->geo->draw_args["grid"].n_index;
        wave_ritem->start_index = wave_ritem->geo->draw_args["grid"].start_index;
        wave_ritem->base_vertex = wave_ritem->geo->draw_args["grid"].base_vertex;
        wave_ritem->displacement_map_texel_size = { 1.0f / p_wave->ColumnCount(), 1.0f / p_wave->RowCount() };
        wave_ritem->grid_spatial_step = p_wave->SpatialStep();
        ritem_layer[(size_t) RenderLayor::GpuWave].push_back(wave_ritem.get());
        items.push_back(std::move(wave_ritem));

        auto crate_ritem = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&crate_ritem->model, XMMatrixTranslation(3.0f, 2.0f, -9.0f));
        crate_ritem->obj_cb_ind = obj_cb_ind++;
        crate_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        crate_ritem->mat = materials["fence"].get();
        crate_ritem->geo = geometries["box_geo"].get();
        crate_ritem->n_index = crate_ritem->geo->draw_args["box"].n_index;
        crate_ritem->start_index = crate_ritem->geo->draw_args["box"].start_index;
        crate_ritem->base_vertex = crate_ritem->geo->draw_args["box"].base_vertex;
        ritem_layer[(size_t) RenderLayor::AlphaTested].push_back(crate_ritem.get());
        items.push_back(std::move(crate_ritem));
    }

    void DrawRenderItems(ID3D12GraphicsCommandList *cmd_list, const std::vector<RenderItem *> &items) {
        UINT obj_cb_size = D3DUtil::CBSize(sizeof(ObjectConst));
        UINT mat_cb_size = D3DUtil::CBSize(sizeof(MaterialConst));
        auto obj_cb = curr_fr->p_obj_cb->Resource();
        auto mat_cb = curr_fr->p_mat_cb->Resource();
        for (auto item : items) { // per object
            // set vb, ib and primitive type
            auto vbv = item->geo->VertexBufferView();
            auto ibv = item->geo->IndexBufferView();
            cmd_list->IASetVertexBuffers(0, 1, &vbv);
            cmd_list->IASetIndexBuffer(&ibv);
            cmd_list->IASetPrimitiveTopology(item->prim_ty);

            // set per object cbv
            auto obj_cb_addr = obj_cb->GetGPUVirtualAddress() + item->obj_cb_ind * obj_cb_size;
            cmd_list->SetGraphicsRootConstantBufferView(0, obj_cb_addr);
            // set matertial cbv
            auto mat_cb_addr = mat_cb->GetGPUVirtualAddress() + item->mat->mat_cb_ind * mat_cb_size;
            cmd_list->SetGraphicsRootConstantBufferView(1, mat_cb_addr);
            // set texture srv (descriptor table)
            auto diffuse_tex = CD3DX12_GPU_DESCRIPTOR_HANDLE(p_cbv_srv_uav_heap->GetGPUDescriptorHandleForHeapStart(),
                item->mat->diffuse_srv_heap_index, cbv_srv_uav_descriptor_size);
            cmd_list->SetGraphicsRootDescriptorTable(3, diffuse_tex);

            // draw
            cmd_list->DrawIndexedInstanced(item->n_index, 1, item->start_index, item->base_vertex, 0);
        }
    }
    
    float GetHillHeight(float x, float z) const {
        return 0.3f * (z * std::sin(0.1f * x) + x * std::cos(0.1f * z));
    }
    XMFLOAT3 GetHillNormal(float x, float z) const {
        // n = (-df/dx, 1, -df/dz)
        XMFLOAT3 norm(
            -0.03f * z * std::cos(0.1f * x) - 0.3f * std::cosf(0.1f * z),
            1.0f,
            -0.3f * std::sin(0.1f * x) + 0.03f * x * std::sin(0.1f * z)
        );

        XMStoreFloat3(&norm, XMVector3Normalize(XMLoadFloat3(&norm)));
        return norm;
    }

    std::vector<std::unique_ptr<FrameResource>> frame_resources;
    FrameResource *curr_fr = nullptr;
    int curr_fr_ind = 0;

    ComPtr<ID3D12RootSignature> p_rt_sig = nullptr;
    ComPtr<ID3D12RootSignature> p_wave_rt_sig = nullptr;
    ComPtr<ID3D12RootSignature> p_post_rt_sig = nullptr;
    ComPtr<ID3D12DescriptorHeap> p_cbv_srv_uav_heap;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> geometries;
    std::unordered_map<std::string, std::unique_ptr<Material>> materials;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textures;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> psos;

    std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;

    std::vector<std::unique_ptr<RenderItem>> items;
    std::vector<RenderItem *> ritem_layer[(size_t) RenderLayor::Count];
    std::unique_ptr<Wave> p_wave;
    std::unique_ptr<BlurFilter> p_blur_filter;

    PassConst main_pass_cb;

    XMFLOAT3 eye = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4X4 view = DXMath::Identity4x4();
    XMFLOAT4X4 proj = DXMath::Identity4x4();

    float theta = 0.0f;
    float phi = XM_PIDIV2;
    float radius = 25.0f;

    POINT last_mouse;
};

int WINAPI WinMain(HINSTANCE h_inst, HINSTANCE h_prev_inst, PSTR cmd_line, int show_cmd) {
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try {
        D3DAppBlur d3d_app(h_inst);
        if (!d3d_app.Initialize()) {
            return 0;
        }
        return d3d_app.Run();
    } catch (DxException &e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}