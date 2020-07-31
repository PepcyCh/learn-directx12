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

using Microsoft::WRL::ComPtr;
using namespace DirectX;

const int n_frame_resource = 3;

struct RenderItem {
    XMFLOAT4X4 model = DXMath::Identity4x4();
    XMFLOAT4X4 tex_transform = DXMath::Identity4x4();
    int n_frame_dirty = n_frame_resource;
    UINT obj_cb_ind = -1;
    MeshGeometry *geo = nullptr;
    Material *mat = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT n_index = 0;
    UINT start_index = 0;
    int base_vertex = 0;
};

enum class RenderLayor : size_t {
    Opaque,
    Count
};

// use HS & DS to tessellate a quad according to distance to eye
// and displace the vertices to be a hill
// use '1' to switch on/off wireframe

class D3DAppTSBaisc : public D3DApp {
  public:
    D3DAppTSBaisc(HINSTANCE h_inst) : D3DApp(h_inst) {}
    ~D3DAppTSBaisc() {
        if (p_device != nullptr) {
            FlushCommandQueue();
        }
    }

    bool Initialize() override {
        if (!D3DApp::Initialize()) {
            return false;
        }

        ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), nullptr));

        LoadTextures();
        BuildRootSignature();
        BuildDescriptorHeaps();
        BuildShaderAndInputLayout();
        BuildLandGeometry();
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
        if (b_wireframe) {
            ThrowIfFailed(p_cmd_list->Reset(cmd_alloc.Get(), psos["wireframe"].Get()));
        } else {
            ThrowIfFailed(p_cmd_list->Reset(cmd_alloc.Get(), psos["opaque"].Get()));
        }

        // viewport and scissor
        p_cmd_list->RSSetViewports(1, &viewport);
        p_cmd_list->RSSetScissorRects(1, &scissors);

        // back buffer: present -> render target
        auto transit_barrier = CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        p_cmd_list->ResourceBarrier(1, &transit_barrier);

        // clear rtv and dsv
        auto back_buffer_view = CurrBackBufferView();
        auto depth_stencil_view = DepthStencilView();
        p_cmd_list->ClearRenderTargetView(back_buffer_view, (float *) &main_pass_cb.fog_color, 0, nullptr);
        p_cmd_list->ClearDepthStencilView(depth_stencil_view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f, 0, 0, nullptr);
        // set rtv and dsv
        p_cmd_list->OMSetRenderTargets(1, &back_buffer_view, true, &depth_stencil_view);

        // set root signature
        p_cmd_list->SetGraphicsRootSignature(p_rt_sig.Get());
        // set cbv/srv/uav heaps
        ID3D12DescriptorHeap *heaps[] = { p_srv_heap.Get() };
        p_cmd_list->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);
        // set per pass cbv
        auto pass_cb = curr_fr->p_pass_cb->Resource();
        p_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());

        // draw opaque items
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::Opaque]);

        // back buffer: render target -> present
        transit_barrier = CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        p_cmd_list->ResourceBarrier(1, &transit_barrier);
        
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
        if (GetAsyncKeyState('1') & 0x8000) {
            b_wireframe = !b_wireframe;
        }
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
        main_pass_cb.lights[0].strength = { 0.9f, 0.9f, 0.9f };
        main_pass_cb.lights[1].direction = { -0.57735f, -0.57735f, 0.57735f };
        main_pass_cb.lights[1].strength = { 0.5f, 0.5f, 0.5f };
        main_pass_cb.lights[2].direction = { 0.0f, -0.707f, -0.707f };
        main_pass_cb.lights[2].strength = { 0.2f, 0.2f, 0.2f };

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
    void AnimateMaterials(const Timer &timer) {
        ;
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

        auto white_tex = std::make_unique<Texture>();
        white_tex->name = "white";
        white_tex->filename = root_path + L"textures/white1x1.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            white_tex->filename.c_str(), &white_tex->resource));
        textures[white_tex->name] = std::move(white_tex);

        auto grass_tex = std::make_unique<Texture>();
        grass_tex->name = "grass";
        grass_tex->filename = root_path + L"textures/grass.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            grass_tex->filename.c_str(), &grass_tex->resource));
        textures[grass_tex->name] = std::move(grass_tex);

        // do copy & flush
        // upload_finish is std::future<void>
        auto upload_finish = resource_upload.End(p_cmd_queue.Get());
        FlushCommandQueue();
        upload_finish.wait();
    }

    void BuildRootSignature() {
        CD3DX12_ROOT_PARAMETER rt_params[4];
        rt_params[0].InitAsConstantBufferView(0);
        rt_params[1].InitAsConstantBufferView(1);
        rt_params[2].InitAsConstantBufferView(2);
        auto srv_range = CD3DX12_DESCRIPTOR_RANGE(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
        rt_params[3].InitAsDescriptorTable(1, &srv_range, D3D12_SHADER_VISIBILITY_PIXEL);

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
    void BuildDescriptorHeaps() {
        D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
        srv_heap_desc.NumDescriptors = textures.size();
        srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(p_device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&p_srv_heap)));

        CD3DX12_CPU_DESCRIPTOR_HANDLE h_srv(p_srv_heap->GetCPUDescriptorHandleForHeapStart());
        auto white_tex = textures["white"]->resource;
        auto grass_tex = textures["grass"]->resource;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = white_tex->GetDesc().Format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = -1;
        p_device->CreateShaderResourceView(white_tex.Get(), &srv_desc, h_srv);

        h_srv.Offset(1, cbv_srv_uav_descriptor_size);
        srv_desc.Format = grass_tex->GetDesc().Format;
        p_device->CreateShaderResourceView(grass_tex.Get(), &srv_desc, h_srv);
    }
    void BuildShaderAndInputLayout() {
        // build shader from hlsl file
        shaders["tess_vs"] = D3DUtil::CompileShader(src_path + L"ch14_tessellation_basic/shaders/tessellation.hlsl",
            nullptr, "VS", "vs_5_1");
        shaders["tess_hs"] = D3DUtil::CompileShader(src_path + L"ch14_tessellation_basic/shaders/tessellation.hlsl",
            nullptr, "HS", "hs_5_1");
        shaders["tess_ds"] = D3DUtil::CompileShader(src_path + L"ch14_tessellation_basic/shaders/tessellation.hlsl",
            nullptr, "DS", "ds_5_1");
        shaders["tess_ps"] = D3DUtil::CompileShader(src_path + L"ch14_tessellation_basic/shaders/tessellation.hlsl",
            nullptr, "PS", "ps_5_1");

        // input layout and input elements specify input of (vertex) shader
        std_input_layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
    }
    void BuildLandGeometry() {
        std::array<XMFLOAT3, 4> vertices = {
            XMFLOAT3(-10.0f, 0.0f, -10.0f),
            XMFLOAT3(-10.0f, 0.0f,  10.0f),
            XMFLOAT3( 10.0f, 0.0f, -10.0f),
            XMFLOAT3( 10.0f, 0.0f,  10.0f)
        };

        std::array<uint16_t, 4> indices = { 0, 1, 2, 3 };

        const UINT vb_size = vertices.size() * sizeof(XMFLOAT3);
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

        geo->vb_stride = sizeof(XMFLOAT3);
        geo->vb_size = vb_size;
        geo->index_fmt = DXGI_FORMAT_R16_UINT;
        geo->ib_size = ib_size;

        SubmeshGeometry submesh;
        submesh.n_index = indices.size();
        submesh.start_index = 0;
        submesh.base_vertex = 0;
        geo->draw_args["quad_patch"] = submesh;

        geometries[geo->name] = std::move(geo);
    }
    void BuildMaterials() {
        int mat_cb_ind = 0;

        auto white = std::make_unique<Material>();
        white->name = "white";
        white->n_frame_dirty = n_frame_resource;
        white->mat_cb_ind = mat_cb_ind++;
        white->diffuse_srv_heap_index = 0;
        white->albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        white->fresnel_r0 = { 0.01f, 0.01f, 0.01f };
        white->roughness = 0.125f;
        materials[white->name] = std::move(white);
        
        auto grass = std::make_unique<Material>();
        grass->name = "grass";
        grass->n_frame_dirty = n_frame_resource;
        grass->mat_cb_ind = mat_cb_ind++;
        grass->diffuse_srv_heap_index = 1;
        grass->albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        grass->fresnel_r0 = { 0.01f, 0.01f, 0.01f };
        grass->roughness = 0.125f;
        materials[grass->name] = std::move(grass);
    }
    void BuildPSOs() {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC opaque_pso_desc = {};
        opaque_pso_desc.InputLayout = { std_input_layout.data(), (UINT) std_input_layout.size() };
        opaque_pso_desc.pRootSignature = p_rt_sig.Get();
        opaque_pso_desc.VS = {
            reinterpret_cast<BYTE *>(shaders["tess_vs"]->GetBufferPointer()),
            shaders["tess_vs"]->GetBufferSize()
        };
        opaque_pso_desc.HS = {
            reinterpret_cast<BYTE *>(shaders["tess_hs"]->GetBufferPointer()),
            shaders["tess_hs"]->GetBufferSize()
        };
        opaque_pso_desc.DS = {
            reinterpret_cast<BYTE *>(shaders["tess_ds"]->GetBufferPointer()),
            shaders["tess_ds"]->GetBufferSize()
        };
        opaque_pso_desc.PS = {
            reinterpret_cast<BYTE *>(shaders["tess_ps"]->GetBufferPointer()),
            shaders["tess_ps"]->GetBufferSize()
        };
        opaque_pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        opaque_pso_desc.RasterizerState.FrontCounterClockwise = true; // ccw front face
        opaque_pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        opaque_pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        opaque_pso_desc.SampleMask = UINT_MAX;
        opaque_pso_desc.SampleDesc.Count = 1;
        opaque_pso_desc.SampleDesc.Quality = 0;
        opaque_pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_PATCH;
        opaque_pso_desc.NumRenderTargets = 1;
        opaque_pso_desc.RTVFormats[0] = back_buffer_fmt;
        opaque_pso_desc.DSVFormat = depth_stencil_fmt;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&opaque_pso_desc,
            IID_PPV_ARGS(&psos["opaque"])));

        D3D12_GRAPHICS_PIPELINE_STATE_DESC wireframe_pso_desc = opaque_pso_desc;
        wireframe_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&wireframe_pso_desc,
            IID_PPV_ARGS(&psos["wireframe"])));
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
        grid_ritem->obj_cb_ind = obj_cb_ind++;
        grid_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        grid_ritem->mat = materials["grass"].get();
        grid_ritem->geo = geometries["land_geo"].get();
        grid_ritem->n_index = grid_ritem->geo->draw_args["quad_patch"].n_index;
        grid_ritem->start_index = grid_ritem->geo->draw_args["quad_patch"].start_index;
        grid_ritem->base_vertex = grid_ritem->geo->draw_args["quad_patch"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Opaque].push_back(grid_ritem.get());
        items.push_back(std::move(grid_ritem));
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
            auto diffuse_tex = CD3DX12_GPU_DESCRIPTOR_HANDLE(p_srv_heap->GetGPUDescriptorHandleForHeapStart(),
                item->mat->diffuse_srv_heap_index, cbv_srv_uav_descriptor_size);
            cmd_list->SetGraphicsRootDescriptorTable(3, diffuse_tex);

            // draw
            cmd_list->DrawIndexedInstanced(item->n_index, 1, item->start_index, item->base_vertex, 0);
        }
    }

    std::vector<std::unique_ptr<FrameResource>> frame_resources;
    FrameResource *curr_fr = nullptr;
    int curr_fr_ind = 0;

    ComPtr<ID3D12RootSignature> p_rt_sig = nullptr;
    ComPtr<ID3D12DescriptorHeap> p_srv_heap;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> geometries;
    std::unordered_map<std::string, std::unique_ptr<Material>> materials;
    std::unordered_map<std::string, std::unique_ptr<Texture>> textures;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> psos;

    std::vector<D3D12_INPUT_ELEMENT_DESC> std_input_layout;
    std::vector<D3D12_INPUT_ELEMENT_DESC> tree_sprite_input_layout;

    std::vector<std::unique_ptr<RenderItem>> items;
    std::vector<RenderItem *> ritem_layer[(size_t) RenderLayor::Count];

    PassConst main_pass_cb;

    bool b_wireframe = true;

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
        D3DAppTSBaisc d3d_app(h_inst);
        if (!d3d_app.Initialize()) {
            return 0;
        }
        return d3d_app.Run();
    } catch (DxException &e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}