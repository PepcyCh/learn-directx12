#include <algorithm>

#include <d3dcompiler.h>
#include <DirectXColors.h>

#include "../defines.h"
#include "D3DApp.h"
#include "D3DUtil.h"
#include "GeometryGenerator.h"
#include "FrameResource.h"
#include "Wave.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

const int n_frame_resource = 3;

struct RenderItem {
    XMFLOAT4X4 model = DXMath::Identity4x4();
    int n_frame_dirty = n_frame_resource;
    UINT obj_cb_ind = -1;
    MeshGeometry *geo = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT n_index = 0;
    UINT start_index = 0;
    int base_vertex = 0;
};

enum class RenderLayor : size_t {
    Opaque,
    Count
};

// use root descriptors instead of descriptor tables in root signature

class D3DAppLandWave : public D3DApp {
  public:
    D3DAppLandWave(HINSTANCE h_inst) : D3DApp(h_inst) {}
    ~D3DAppLandWave() {
        if (p_device != nullptr) {
            FlushCommandQueue();
        }
    }

    bool Initialize() override {
        if (!D3DApp::Initialize()) {
            return false;
        }

        p_wave = std::make_unique<Wave>(128, 128, 1.0f, 0.03f, 4.0f, 0.2f);

        ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), nullptr));

        BuildRootSignature();
        BuildShaderAndInputLayout();
        BuildLandGeometry();
        BuildWaveGeometryBuffers();
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
        XMMATRIX _proj = XMMatrixPerspectiveFovRH(XM_PIDIV4, Aspect(), 0.1f, 100.0f);
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

        UpdateWaves(timer);
        UpdateObjCont(timer);
        UpdatePassConst(timer);
    }
    void Draw(const Timer &timer) override {
        // reset cmd list and cmd alloc
        auto cmd_alloc = curr_fr->p_cmd_alloc;
        ThrowIfFailed(cmd_alloc->Reset());
        if (wire_frame) {
            ThrowIfFailed(p_cmd_list->Reset(cmd_alloc.Get(), psos["opaque_wf"].Get()));
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
        p_cmd_list->ClearRenderTargetView(back_buffer_view, Colors::LightBlue, 0, nullptr);
        p_cmd_list->ClearDepthStencilView(depth_stencil_view, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f, 0, 0, nullptr);
        // set rtv and dsv
        p_cmd_list->OMSetRenderTargets(1, &back_buffer_view, true, &depth_stencil_view);

        // set root signature
        p_cmd_list->SetGraphicsRootSignature(p_rt_sig.Get());
        // set per pass cbv
        auto pass_cb = curr_fr->p_pass_cb->Resource();
        p_cmd_list->SetGraphicsRootConstantBufferView(1, pass_cb->GetGPUVirtualAddress());

        // draw items
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
            float dx = 0.005f * (x - last_mouse.x);
            float dy = 0.005f * (y - last_mouse.y);
            radius += dx - dy;
            radius = std::clamp(radius, 3.0f, 25.0f);
        }
        last_mouse.x = x;
        last_mouse.y = y;
    }
    void OnMouseUp(WPARAM btn_state, int x, int y) override {
        ReleaseCapture();
    }

    void OnKeyboardInput(const Timer &timer) {
        if (GetAsyncKeyState('1') & 0x8000) {
            wire_frame = !wire_frame;
        }
    }
    void UpdateCamera(const Timer &timer) {
        float x = radius * std::sin(phi) * std::cos(theta);
        float y = radius * std::cos(phi);
        float z = radius * std::sin(phi) * std::sin(theta);

        XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
        XMVECTOR lookat = XMVectorZero();
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMMATRIX _view = XMMatrixLookAtRH(pos, lookat, up);
        XMStoreFloat4x4(&view, _view);
    }
    void UpdateObjCont(const Timer &timer) {
        auto obj_cb_upd = curr_fr->p_obj_cb.get();
        for (auto &item : items) {
            if (item->n_frame_dirty > 0) {
                XMMATRIX model = XMLoadFloat4x4(&item->model);
                ObjectConst obj_const;
                XMStoreFloat4x4(&obj_const.model, model);
                obj_cb_upd->CopyData(item->obj_cb_ind, obj_const);
                --item->n_frame_dirty;
            }
        }
    }
    void UpdatePassConst(const Timer &timer) {
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
        main_pass_cb.near_z = 0.1f;
        main_pass_cb.far_z = 100.0f;
        main_pass_cb.rt_size = { (float) client_width, (float) client_height };
        main_pass_cb.rt_size_inv = { 1.0f / client_width, 1.0f / client_height };
        main_pass_cb.delta_time = timer.DeltaTime();
        main_pass_cb.total_time = timer.TotalTime();

        auto curr_pass_cb = curr_fr->p_pass_cb.get();
        curr_pass_cb->CopyData(0, main_pass_cb);
    }
    void UpdateWaves(const Timer &timer) {
        // Every quarter second, generate a random wave.
        static float t_base = 0.0f;
        if ((this->timer.TotalTime() - t_base) >= 0.25f) {
            t_base += 0.25f;

            int i = DXMath::RandI(4, p_wave->RowCount() - 5);
            int j = DXMath::RandI(4, p_wave->ColumnCount() - 5);

            float r = DXMath::RandF(0.2f, 0.5f);

            p_wave->Disturb(i, j, r);
        }

        // Update the wave simulation.
        p_wave->Update(timer.DeltaTime());

        // Update the wave vertex buffer with the new solution.
        auto wave_vb = curr_fr->p_wave_vb.get();
        for (int i = 0; i < p_wave->VertexCount(); i++) {
            Vertex v;

            v.pos = p_wave->Position(i);
            v.color = XMFLOAT4(DirectX::Colors::Blue);

            wave_vb->CopyData(i, v);
        }

        // Set the dynamic VB of the wave renderitem to the current frame VB.
        wave_ritem->geo->vb_gpu = wave_vb->Resource();
    }

    void BuildRootSignature() {
        CD3DX12_ROOT_PARAMETER rt_params[2];
        rt_params[0].InitAsConstantBufferView(0);
        rt_params[1].InitAsConstantBufferView(1);

        CD3DX12_ROOT_SIGNATURE_DESC rt_sig_desc(2, rt_params, 0, nullptr,
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
    void BuildShaderAndInputLayout() {
        // build shader from hlsl file
        shaders["standard_vs"] = D3DUtil::CompileShader(src_path + L"ch07_land_wave/shaders/shape.hlsl",
            nullptr, "VS", "vs_5_1");
        shaders["opaque_ps"] = D3DUtil::CompileShader(src_path + L"ch07_land_wave/shaders/shape.hlsl",
            nullptr, "PS", "ps_5_1");

        // input layout and input elements specify input of (vertex) shader
        input_layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
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

            if (vertices[i].pos.y < -10.0f) {
                vertices[i].color = XMFLOAT4(1.0f, 0.96f, 0.62f, 1.0f);
            } else if (vertices[i].pos.y < 5.0f) {
                vertices[i].color = XMFLOAT4(0.48f, 0.77f, 0.46f, 1.0f);
            } else if (vertices[i].pos.y < 12.0f) {
                vertices[i].color = XMFLOAT4(0.1f, 0.48f, 0.19f, 1.0f);
            } else if (vertices[i].pos.y < 20.0f) {
                vertices[i].color = XMFLOAT4(0.45f, 0.39f, 0.34f, 1.0f);
            } else {
                vertices[i].color = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
            }
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
        geometries["land_geo"] = std::move(geo);
    }
    void BuildWaveGeometryBuffers() {
        std::vector<uint16_t> indices(3 * p_wave->TriangleCount());
        assert(p_wave->VertexCount() < 0x0000ffff);

        // Iterate over each quad.
        int m = p_wave->RowCount();
        int n = p_wave->ColumnCount();
        int k = 0;
        for (int i = 0; i < m - 1; i++) {
            for (int j = 0; j < n - 1; j++) {
                indices[k] = i * n + j;
                indices[k + 1] = i * n + j + 1;
                indices[k + 2] = (i + 1) * n + j;
                indices[k + 3] = (i + 1) * n + j;
                indices[k + 4] = i * n + j + 1;
                indices[k + 5] = (i + 1) * n + j + 1;
                k += 6;
            }
        }

        UINT vb_size = p_wave->VertexCount() * sizeof(Vertex);
        UINT ib_size = indices.size() * sizeof(uint16_t);

        auto geo = std::make_unique<MeshGeometry>();
        geo->name = "water_geo";

        // Set dynamically.
        geo->vb_cpu = nullptr;
        geo->vb_gpu = nullptr;

        ThrowIfFailed(D3DCreateBlob(ib_size, &geo->ib_cpu));
        CopyMemory(geo->ib_cpu->GetBufferPointer(), indices.data(), ib_size);

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

        geometries["water_geo"] = std::move(geo);
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

        D3D12_GRAPHICS_PIPELINE_STATE_DESC opaque_wf_pso_desc = opaque_pso_desc;
        opaque_wf_pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME; // wire frame mode
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&opaque_wf_pso_desc, IID_PPV_ARGS(&psos["opaque_wf"])));
    }
    void BuildFrameResources() {
        for (int i = 0; i < n_frame_resource; i++) {
            frame_resources.push_back(std::make_unique<FrameResource>(p_device.Get(), 1, items.size(),
                p_wave->VertexCount()));
        }
    }
    void BuildRenderItems() {
        auto wave_ritem = std::make_unique<RenderItem>();
        wave_ritem->model = DXMath::Identity4x4();
        wave_ritem->obj_cb_ind = 0;
        wave_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        wave_ritem->geo = geometries["water_geo"].get();
        wave_ritem->n_index = wave_ritem->geo->draw_args["grid"].n_index;
        wave_ritem->start_index = wave_ritem->geo->draw_args["grid"].start_index;
        wave_ritem->base_vertex = wave_ritem->geo->draw_args["grid"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Opaque].push_back(wave_ritem.get());
        this->wave_ritem = wave_ritem.get();

        auto grid_ritem = std::make_unique<RenderItem>();
        grid_ritem->model = DXMath::Identity4x4();
        grid_ritem->obj_cb_ind = 1;
        grid_ritem->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        grid_ritem->geo = geometries["land_geo"].get();
        grid_ritem->n_index = grid_ritem->geo->draw_args["grid"].n_index;
        grid_ritem->start_index = grid_ritem->geo->draw_args["grid"].start_index;
        grid_ritem->base_vertex = grid_ritem->geo->draw_args["grid"].base_vertex;
        ritem_layer[(size_t) RenderLayor::Opaque].push_back(grid_ritem.get());

        items.push_back(std::move(wave_ritem));
        items.push_back(std::move(grid_ritem));
    }

    void DrawRenderItems(ID3D12GraphicsCommandList *cmd_list, const std::vector<RenderItem *> &items) {
        UINT obj_cb_size = D3DUtil::CBSize(sizeof(ObjectConst));
        auto obj_cb = curr_fr->p_obj_cb->Resource();
        for (auto item : items) { // per object
            // set vb, ib and primitive type
            auto vbv = item->geo->VertexBufferView();
            auto ibv = item->geo->IndexBufferView();
            cmd_list->IASetVertexBuffers(0, 1, &vbv);
            cmd_list->IASetIndexBuffer(&ibv);
            cmd_list->IASetPrimitiveTopology(item->prim_ty);

            // set per object cbv
            auto obj_cb_addr = obj_cb->GetGPUVirtualAddress();
            obj_cb_addr += item->obj_cb_ind * obj_cb_size;
            cmd_list->SetGraphicsRootConstantBufferView(0, obj_cb_addr);

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

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> geometries;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> psos;

    std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;

    std::vector<std::unique_ptr<RenderItem>> items;
    RenderItem *wave_ritem;
    std::vector<RenderItem *> ritem_layer[(size_t) RenderLayor::Count];
    std::unique_ptr<Wave> p_wave;

    PassConst main_pass_cb;

    bool wire_frame = false;

    XMFLOAT3 eye = { 0.0f, 0.0f, 0.0f };
    XMFLOAT4X4 view = DXMath::Identity4x4();
    XMFLOAT4X4 proj = DXMath::Identity4x4();

    float theta = 0.0f;
    float phi = XM_PIDIV2;
    float radius = 15.0f;

    POINT last_mouse;
};

int WINAPI WinMain(HINSTANCE h_inst, HINSTANCE h_prev_inst, PSTR cmd_line, int show_cmd) {
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try {
        D3DAppLandWave d3d_app(h_inst);
        if (!d3d_app.Initialize()) {
            return 0;
        }
        return d3d_app.Run();
    } catch (DxException &e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}