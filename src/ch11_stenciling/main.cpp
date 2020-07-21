#include <DirectXMath.h>
#include <array>
#include <algorithm>

#include <combaseapi.h>
#include <cstddef>
#include <d3d12.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <DirectXColors.h>
#include <debugapi.h>
#include <fstream>
#include <memory>
#include <minwinbase.h>
#include <stdint.h>
#include <string>
#include <type_traits>
#include <winuser.h>

#include "DXMath.h"
#include "ResourceUploadBatch.h"
#include "DDSTextureLoader.h"

#include "../defines.h"
#include "D3DApp.h"
#include "D3DUtil.h"
#include "GeometryGenerator.h"
#include "FrameResource.h"
#include "d3dx12.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

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
};

enum class RenderLayor : size_t {
    Opaque,      // opaque models
    Mirror,      // stencil
    Reflected,   // reflected models
    Transparent, // transparent models that need blending
    Count
};

// no shadow matrix shadow
// add reflected floor

class D3DAppStenciling : public D3DApp {
  public:
    D3DAppStenciling(HINSTANCE h_inst) : D3DApp(h_inst) {}
    ~D3DAppStenciling() {
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
        BuildRoomGeometry();
        BuildSkullGeometry();
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
        UpdateMainPassCB(timer);
        UpdateReflectedPassCB(timer);
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

        // draw opaque items
        UINT pass_cb_size = D3DUtil::CBSize(sizeof(PassConst));
        auto pass_cb = curr_fr->p_pass_cb->Resource();
        p_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::Opaque]);

        // draw mirror (mark stencil)
        p_cmd_list->OMSetStencilRef(1);
        p_cmd_list->SetPipelineState(psos["stencil_mirror"].Get());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::Mirror]);

        // draw reflected items
        p_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress() + pass_cb_size);
        p_cmd_list->SetPipelineState(psos["stencil_reflect"].Get());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::Reflected]);

        // draw transparent items
        p_cmd_list->SetGraphicsRootConstantBufferView(2, pass_cb->GetGPUVirtualAddress());
        p_cmd_list->OMSetStencilRef(0);
        p_cmd_list->SetPipelineState(psos["transparent"].Get());
        DrawRenderItems(p_cmd_list.Get(), ritem_layer[(size_t) RenderLayor::Transparent]);

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
        float dt = timer.DeltaTime();

        if (GetAsyncKeyState('A') & 0x8000) {
            skull_translation.x += 1.0f * dt;
        }
        if (GetAsyncKeyState('D') & 0x8000) {
            skull_translation.x -= 1.0f * dt;
        }
        if (GetAsyncKeyState('W') & 0x8000) {
            skull_translation.y += 1.0f * dt;
        }
        if (GetAsyncKeyState('S') & 0x8000) {
            skull_translation.y -= 1.0f * dt;
        }
        skull_translation.y = std::max(skull_translation.y, 0.0f);

        XMMATRIX rotate = XMMatrixRotationY(0.5 * XM_PI);
        XMMATRIX scale = XMMatrixScaling(0.45f, 0.45f, 0.45f);
        XMMATRIX translate = XMMatrixTranslation(skull_translation.x, skull_translation.y, skull_translation.z);
        XMMATRIX model = rotate * scale * translate;
        XMStoreFloat4x4(&p_skull_ritem->model, model);

        XMVECTOR mirror_plane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        XMMATRIX reflect = XMMatrixReflect(mirror_plane);
        XMStoreFloat4x4(&p_reflected_skull_ritem->model, model * reflect);

        p_skull_ritem->n_frame_dirty = n_frame_resource;
        p_reflected_skull_ritem->n_frame_dirty = n_frame_resource;
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
    void UpdateMainPassCB(const Timer &timer) {
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
    void UpdateReflectedPassCB(const Timer &timer) {
        reflected_pass_cb = main_pass_cb;

        XMVECTOR mirror_plane = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
        XMMATRIX reflect = XMMatrixReflect(mirror_plane);

        for (int i = 0; i < 3; i++) {
            XMVECTOR light_dir = XMLoadFloat3(&main_pass_cb.lights[i].direction);
            XMVECTOR reflected_dir = XMVector3TransformNormal(light_dir, reflect);
            XMStoreFloat3(&reflected_pass_cb.lights[i].direction, reflected_dir);
        }

        auto pass_cb = curr_fr->p_pass_cb.get();
        pass_cb->CopyData(1, reflected_pass_cb);
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

        auto bricks_tex = std::make_unique<Texture>();
        bricks_tex->name = "bricks";
        bricks_tex->filename = root_path + L"textures/bricks3.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            bricks_tex->filename.c_str(), &bricks_tex->resource));
        textures[bricks_tex->name] = std::move(bricks_tex);

        auto checkboard_tex = std::make_unique<Texture>();
        checkboard_tex->name = "checkboard";
        checkboard_tex->filename = root_path + L"textures/checkboard.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            checkboard_tex->filename.c_str(), &checkboard_tex->resource));
        textures[checkboard_tex->name] = std::move(checkboard_tex);

        auto ice_tex = std::make_unique<Texture>();
        ice_tex->name = "ice";
        ice_tex->filename = root_path + L"textures/ice.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            ice_tex->filename.c_str(), &ice_tex->resource));
        textures[ice_tex->name] = std::move(ice_tex);

        auto white_tex = std::make_unique<Texture>();
        white_tex->name = "white";
        white_tex->filename = root_path + L"textures/white1x1.dds";
        ThrowIfFailed(CreateDDSTextureFromFile(p_device.Get(), resource_upload,
            white_tex->filename.c_str(), &white_tex->resource));
        textures[white_tex->name] = std::move(white_tex);

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
        auto bricks_tex = textures["bricks"]->resource;
        auto checkboard_tex = textures["checkboard"]->resource;
        auto ice_tex = textures["ice"]->resource;
        auto white_tex = textures["white"]->resource;

        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Format = bricks_tex->GetDesc().Format;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MostDetailedMip = 0;
        srv_desc.Texture2D.MipLevels = -1;
        p_device->CreateShaderResourceView(bricks_tex.Get(), &srv_desc, h_srv);

        h_srv.Offset(1, cbv_srv_uav_descriptor_size);
        srv_desc.Format = checkboard_tex->GetDesc().Format;
        p_device->CreateShaderResourceView(checkboard_tex.Get(), &srv_desc, h_srv);

        h_srv.Offset(1, cbv_srv_uav_descriptor_size);
        srv_desc.Format = ice_tex->GetDesc().Format;
        p_device->CreateShaderResourceView(ice_tex.Get(), &srv_desc, h_srv);

        h_srv.Offset(1, cbv_srv_uav_descriptor_size);
        srv_desc.Format = white_tex->GetDesc().Format;
        p_device->CreateShaderResourceView(white_tex.Get(), &srv_desc, h_srv);
    }
    void BuildShaderAndInputLayout() {
        // defines
        const D3D_SHADER_MACRO fog_defines[] = {
            "FOG", "1",
            nullptr, nullptr
        };

        // build shader from hlsl file
        shaders["standard_vs"] = D3DUtil::CompileShader(src_path + L"ch11_stenciling/shaders/P3N3U2_default.hlsl",
            nullptr, "VS", "vs_5_1");
        shaders["opaque_ps"] = D3DUtil::CompileShader(src_path + L"ch11_stenciling/shaders/P3N3U2_default.hlsl",
            fog_defines, "PS", "ps_5_1");

        // input layout and input elements specify input of (vertex) shader
        input_layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
    }
    void BuildRoomGeometry() {
        std::array<Vertex, 20> vertices = {
            // Floor: Observe we tile texture coordinates.
            Vertex(-3.5f, 0.0f, -10.0f, 0.0f, 1.0f, 0.0f, 0.0f, 4.0f), // 0 
            Vertex(-3.5f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f),
            Vertex( 7.5f, 0.0f,   0.0f, 0.0f, 1.0f, 0.0f, 4.0f, 0.0f),
            Vertex( 7.5f, 0.0f, -10.0f, 0.0f, 1.0f, 0.0f, 4.0f, 4.0f),

            // Wall: Observe we tile texture coordinates, and that we
            // leave a gap in the middle for the mirror.
            Vertex(-3.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 2.0f), // 4
            Vertex(-3.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
            Vertex(-2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.5f, 0.0f),
            Vertex(-2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.5f, 2.0f),

            Vertex(2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 2.0f), // 8 
            Vertex(2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
            Vertex(7.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 2.0f, 0.0f),
            Vertex(7.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 2.0f, 2.0f),

            Vertex(-3.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f), // 12
            Vertex(-3.5f, 6.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
            Vertex( 7.5f, 6.0f, 0.0f, 0.0f, 0.0f, -1.0f, 6.0f, 0.0f),
            Vertex( 7.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 6.0f, 1.0f),

            // Mirror
            Vertex(-2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 1.0f), // 16
            Vertex(-2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f),
            Vertex( 2.5f, 4.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f),
            Vertex( 2.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 1.0f, 1.0f)
        };

        std::array<std::int16_t, 30> indices = {
            // Floor
            0, 1, 2,	
            0, 2, 3,

            // Walls
            4, 5, 6,
            4, 6, 7,

            8, 9, 10,
            8, 10, 11,

            12, 13, 14,
            12, 14, 15,

            // Mirror
            16, 17, 18,
            16, 18, 19
        };

        UINT vb_size = vertices.size() * sizeof(Vertex);
        UINT ib_size = indices.size() * sizeof(uint16_t);

        auto geo = std::make_unique<MeshGeometry>();
        geo->name = "room_geo";

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

        SubmeshGeometry floor_submesh;
        floor_submesh.n_index = 6;
        floor_submesh.start_index = 0;
        floor_submesh.base_vertex = 0;
        geo->draw_args["floor"] = floor_submesh;

        SubmeshGeometry wall_submesh;
        wall_submesh.n_index = 18;
        wall_submesh.start_index = 6;
        wall_submesh.base_vertex = 0;
        geo->draw_args["wall"] = wall_submesh;

        SubmeshGeometry mirror_submesh;
        mirror_submesh.n_index = 6;
        mirror_submesh.start_index = 24;
        mirror_submesh.base_vertex = 0;
        geo->draw_args["mirror"] = mirror_submesh;

        geometries[geo->name] = std::move(geo);
    }
    void BuildSkullGeometry() {
        std::ifstream fin(root_path + L"models/skull.txt");
        if (!fin) {
            MessageBox(nullptr, L"skull.txt not found", nullptr, 0);
            return;
        }

        int n_vertex, n_triangle;
        std::string ignore;
        fin >> ignore >> n_vertex;
        fin >> ignore >> n_triangle;
        fin >> ignore >> ignore >> ignore >> ignore;

        std::vector<Vertex> vertices(n_vertex);
        for (int i = 0; i < n_vertex; i++) {
            fin >> vertices[i].pos.x >> vertices[i].pos.y >> vertices[i].pos.z;
            fin >> vertices[i].norm.x >> vertices[i].norm.y >> vertices[i].norm.z;
            vertices[i].texc = { 0.0f, 0.0f };
        }

        fin >> ignore >> ignore >> ignore;
        std::vector<uint16_t> indices(n_triangle * 3);
        for (int i = 0; i < n_triangle; i++) {
            fin >> indices[3 * i] >> indices[3 * i + 1] >> indices[3 * i + 2];
        }
        fin.close();

        UINT vb_size = vertices.size() * sizeof(Vertex);
        UINT ib_size = indices.size() * sizeof(uint16_t);

        auto geo = std::make_unique<MeshGeometry>();
        geo->name = "skull_geo";

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
        geo->draw_args["skull"] = submesh;

        geometries[geo->name] = std::move(geo);
    }
    void BuildMaterials() {
        auto bricks = std::make_unique<Material>();
        bricks->name = "bricks";
        bricks->n_frame_dirty = n_frame_resource;
        bricks->mat_cb_ind = 0;
        bricks->diffuse_srv_heap_index = 0;
        bricks->albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        bricks->fresnel_r0 = { 0.05f, 0.05f, 0.05f };
        bricks->roughness = 0.25f;
        materials[bricks->name] = std::move(bricks);

        auto checkboard = std::make_unique<Material>();
        checkboard->name = "checkboard";
        checkboard->n_frame_dirty = n_frame_resource;
        checkboard->mat_cb_ind = 1;
        checkboard->diffuse_srv_heap_index = 1;
        checkboard->albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        checkboard->fresnel_r0 = { 0.07f, 0.07f, 0.07f };
        checkboard->roughness = 0.3f;
        materials[checkboard->name] = std::move(checkboard);

        auto mirror = std::make_unique<Material>();
        mirror->name = "mirror";
        mirror->n_frame_dirty = n_frame_resource;
        mirror->mat_cb_ind = 2;
        mirror->diffuse_srv_heap_index = 2;
        mirror->albedo = { 1.0f, 1.0f, 1.0f, 0.3f };
        mirror->fresnel_r0 = { 0.1f, 0.1f, 0.1f };
        mirror->roughness = 0.5f;
        materials[mirror->name] = std::move(mirror);

        auto skull = std::make_unique<Material>();
        skull->name = "skull";
        skull->n_frame_dirty = n_frame_resource;
        skull->mat_cb_ind = 3;
        skull->diffuse_srv_heap_index = 3;
        skull->albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
        skull->fresnel_r0 = { 0.05f, 0.05f, 0.05f };
        skull->roughness = 0.3f;
        materials[skull->name] = std::move(skull);
    }
    void BuildPSOs() {
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

        CD3DX12_BLEND_DESC mirror_blend_desc(D3D12_DEFAULT);
        mirror_blend_desc.RenderTarget[0].RenderTargetWriteMask = 0;
        D3D12_DEPTH_STENCIL_DESC mirror_ds_desc = {};
        mirror_ds_desc.DepthEnable = true;
        mirror_ds_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // not draw to the depth buffer
        mirror_ds_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        mirror_ds_desc.StencilEnable = true;
        mirror_ds_desc.StencilWriteMask = 0xff;
        mirror_ds_desc.StencilReadMask = 0xff;
        // stencil - fail
        mirror_ds_desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        // stencil - pass, depth - fail
        mirror_ds_desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        // stencil - pass, depth - pass
        mirror_ds_desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        mirror_ds_desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        // back face is irrelavent since back face is culled
        mirror_ds_desc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        mirror_ds_desc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        mirror_ds_desc.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        mirror_ds_desc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC mirror_pso_desc = opaque_pso_desc;
        mirror_pso_desc.BlendState = mirror_blend_desc;
        mirror_pso_desc.DepthStencilState = mirror_ds_desc;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&mirror_pso_desc,
            IID_PPV_ARGS(&psos["stencil_mirror"])));

        D3D12_DEPTH_STENCIL_DESC reflect_ds_desc;
        reflect_ds_desc.DepthEnable = true;
        reflect_ds_desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        reflect_ds_desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        reflect_ds_desc.StencilEnable = true;
        reflect_ds_desc.StencilWriteMask = 0xff;
        reflect_ds_desc.StencilReadMask = 0xff;
        reflect_ds_desc.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        reflect_ds_desc.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        reflect_ds_desc.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        reflect_ds_desc.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
        // back face is irrelavent since back face is culled
        reflect_ds_desc.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
        reflect_ds_desc.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
        reflect_ds_desc.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        reflect_ds_desc.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC reflect_pso_desc = opaque_pso_desc;
        reflect_pso_desc.DepthStencilState = reflect_ds_desc;
        reflect_pso_desc.RasterizerState.FrontCounterClockwise = false;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&reflect_pso_desc,
            IID_PPV_ARGS(&psos["stencil_reflect"])));
    }
    void BuildFrameResources() {
        for (int i = 0; i < n_frame_resource; i++) {
            frame_resources.push_back(std::make_unique<FrameResource>(p_device.Get(), 2,
                items.size(), materials.size()));
        }
    }
    void BuildRenderItems() {
        int obj_cb_ind = 0;

        auto floor_ritem = std::make_unique<RenderItem>();
        floor_ritem->model = DXMath::Identity4x4();
        floor_ritem->tex_transform = DXMath::Identity4x4();
        floor_ritem->obj_cb_ind = obj_cb_ind++;
        floor_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        floor_ritem->mat = materials["checkboard"].get();
        floor_ritem->geo = geometries["room_geo"].get();
        floor_ritem->n_index = floor_ritem->geo->draw_args["floor"].n_index;
        floor_ritem->start_index = floor_ritem->geo->draw_args["floor"].start_index;
        floor_ritem->base_vertex = floor_ritem->geo->draw_args["floor"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Opaque].push_back(floor_ritem.get());
        items.push_back(std::move(floor_ritem));

        auto reflect_floor_ritem = std::make_unique<RenderItem>();
        XMMATRIX reflect = XMMatrixReflect(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f));
        XMStoreFloat4x4(&reflect_floor_ritem->model, reflect);
        reflect_floor_ritem->tex_transform = DXMath::Identity4x4();
        reflect_floor_ritem->obj_cb_ind = obj_cb_ind++;
        reflect_floor_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        reflect_floor_ritem->mat = materials["checkboard"].get();
        reflect_floor_ritem->geo = geometries["room_geo"].get();
        reflect_floor_ritem->n_index = reflect_floor_ritem->geo->draw_args["floor"].n_index;
        reflect_floor_ritem->start_index = reflect_floor_ritem->geo->draw_args["floor"].start_index;
        reflect_floor_ritem->base_vertex = reflect_floor_ritem->geo->draw_args["floor"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Reflected].push_back(reflect_floor_ritem.get());
        items.push_back(std::move(reflect_floor_ritem));

        auto wall_ritem = std::make_unique<RenderItem>();
        wall_ritem->model = DXMath::Identity4x4();
        wall_ritem->tex_transform = DXMath::Identity4x4();
        wall_ritem->obj_cb_ind = obj_cb_ind++;
        wall_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        wall_ritem->mat = materials["bricks"].get();
        wall_ritem->geo = geometries["room_geo"].get();
        wall_ritem->n_index = wall_ritem->geo->draw_args["wall"].n_index;
        wall_ritem->start_index = wall_ritem->geo->draw_args["wall"].start_index;
        wall_ritem->base_vertex = wall_ritem->geo->draw_args["wall"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Opaque].push_back(wall_ritem.get());
        items.push_back(std::move(wall_ritem));

        auto skull_ritem = std::make_unique<RenderItem>();
        skull_ritem->model = DXMath::Identity4x4(); // will be set in 'OnKeyboardInput()'
        skull_ritem->tex_transform = DXMath::Identity4x4();
        skull_ritem->obj_cb_ind = obj_cb_ind++;
        skull_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        skull_ritem->mat = materials["skull"].get();
        skull_ritem->geo = geometries["skull_geo"].get();
        skull_ritem->n_index = skull_ritem->geo->draw_args["skull"].n_index;
        skull_ritem->start_index = skull_ritem->geo->draw_args["skull"].start_index;
        skull_ritem->base_vertex = skull_ritem->geo->draw_args["skull"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Opaque].push_back(skull_ritem.get());
        p_skull_ritem = skull_ritem.get();
        items.push_back(std::move(skull_ritem));

        auto reflect_skull_ritem = std::make_unique<RenderItem>();
        reflect_skull_ritem->model = DXMath::Identity4x4(); // will be set in 'OnKeyboardInput()'
        reflect_skull_ritem->tex_transform = DXMath::Identity4x4();
        reflect_skull_ritem->obj_cb_ind = obj_cb_ind++;
        reflect_skull_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        reflect_skull_ritem->mat = materials["skull"].get();
        reflect_skull_ritem->geo = geometries["skull_geo"].get();
        reflect_skull_ritem->n_index = reflect_skull_ritem->geo->draw_args["skull"].n_index;
        reflect_skull_ritem->start_index = reflect_skull_ritem->geo->draw_args["skull"].start_index;
        reflect_skull_ritem->base_vertex = reflect_skull_ritem->geo->draw_args["skull"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Reflected].push_back(reflect_skull_ritem.get());
        p_reflected_skull_ritem = reflect_skull_ritem.get();
        items.push_back(std::move(reflect_skull_ritem));

        auto mirror_ritem = std::make_unique<RenderItem>();
        mirror_ritem->model = DXMath::Identity4x4();
        mirror_ritem->tex_transform = DXMath::Identity4x4();
        mirror_ritem->obj_cb_ind = obj_cb_ind++;
        mirror_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        mirror_ritem->mat = materials["mirror"].get();
        mirror_ritem->geo = geometries["room_geo"].get();
        mirror_ritem->n_index = mirror_ritem->geo->draw_args["mirror"].n_index;
        mirror_ritem->start_index = mirror_ritem->geo->draw_args["mirror"].start_index;
        mirror_ritem->base_vertex = mirror_ritem->geo->draw_args["mirrpr"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Mirror].push_back(mirror_ritem.get());
        ritem_layer[(size_t) RenderLayor::Transparent].push_back(mirror_ritem.get());
        items.push_back(std::move(mirror_ritem));
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

    std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;

    std::vector<std::unique_ptr<RenderItem>> items;
    std::vector<RenderItem *> ritem_layer[(size_t) RenderLayor::Count];
    RenderItem *p_skull_ritem = nullptr;
    RenderItem *p_reflected_skull_ritem = nullptr;

    XMFLOAT3 skull_translation = { 0.0f, 1.0f, -5.0f };

    PassConst main_pass_cb;
    PassConst reflected_pass_cb;

    XMFLOAT3 eye = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4X4 view = DXMath::Identity4x4();
    XMFLOAT4X4 proj = DXMath::Identity4x4();

    float theta = 1.25 * XM_PI;
    float phi = XM_PIDIV4;
    float radius = 20.0f;

    POINT last_mouse;
};

int WINAPI WinMain(HINSTANCE h_inst, HINSTANCE h_prev_inst, PSTR cmd_line, int show_cmd) {
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try {
        D3DAppStenciling d3d_app(h_inst);
        if (!d3d_app.Initialize()) {
            return 0;
        }
        return d3d_app.Run();
    } catch (DxException &e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}