#include <algorithm>

#include <d3dcompiler.h>
#include <DirectXColors.h>

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
    int n_frame_dirty = n_frame_resource;
    UINT obj_cb_ind = -1;
    MeshGeometry *geo = nullptr;
    D3D12_PRIMITIVE_TOPOLOGY prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    UINT n_index = 0;
    UINT start_index = 0;
    int base_vertex = 0;
};

// use 'frame resources' to accelerate
// each frame resource holds the constant buffers and a command allocator
// when GPU is processing the commands created via a frame resource's data,
// CPU can still modify data in the other frame resources
// in this way, CPU and GPU can work simultaneously

class D3DAppShape : public D3DApp {
  public:
    D3DAppShape(HINSTANCE h_inst) : D3DApp(h_inst) {}
    ~D3DAppShape() {
        if (p_device != nullptr) {
            FlushCommandQueue();
        }
    }

    bool Initialize() override {
        if (!D3DApp::Initialize()) {
            return false;
        }

        ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), nullptr));

        BuildRootSignature();
        BuildShaderAndInputLayout();
        BuildShapeGeometries();
        BuildRenderItems();
        BuildFrameResources();
        BuildDescriporHeaps();
        BuildCBV();
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
            HANDLE event = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
            ThrowIfFailed(p_fence->SetEventOnCompletion(curr_fr->fence, event));
            WaitForSingleObject(event, INFINITE);
            CloseHandle(event);
        }

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
        p_cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        // clear rtv & dsv
        p_cmd_list->ClearRenderTargetView(CurrBackBufferView(), Colors::LightBlue, 0, nullptr);
        p_cmd_list->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f, 0, 0, nullptr);
        // set render target
        p_cmd_list->OMSetRenderTargets(1, &CurrBackBufferView(), true, &DepthStencilView());

        // set cbv heap & root signature
        ID3D12DescriptorHeap *heaps[] = { p_cbv_heap.Get() };
        p_cmd_list->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);
        p_cmd_list->SetGraphicsRootSignature(p_rt_sig.Get());

        // set per pass cbv
        int pass_cbv_ind = pass_cbv_offset + curr_fr_ind;
        auto h_pass_cbv = CD3DX12_GPU_DESCRIPTOR_HANDLE(p_cbv_heap->GetGPUDescriptorHandleForHeapStart(),
            pass_cbv_ind, cbv_srv_uav_descriptor_size);
        p_cmd_list->SetGraphicsRootDescriptorTable(1, h_pass_cbv);

        // draw items
        DrawRenderItems(p_cmd_list.Get(), opaque_items);

        // back buffer: render target -> present
        p_cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));
        
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
            radius = std::clamp(radius, 3.0f, 15.0f);
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
                // if we use mul(mat, vec) (instead of mul(vec, mat)) in hlsl
                // we don't need to write XMMatrixTranspose here
                // (now we make it the same as what I do in OpenGL program...)
                XMStoreFloat4x4(&obj_const.model, model);
                obj_cb_upd->CopyData(item->obj_cb_ind, obj_const);
                --item->n_frame_dirty;
            }
        }
    }
    void UpdatePassConst(const Timer &timer) {
        XMMATRIX _view = XMLoadFloat4x4(&view);
        XMMATRIX _proj = XMLoadFloat4x4(&proj);

        XMMATRIX _view_inv = XMMatrixInverse(&XMMatrixDeterminant(_view), _view);
        XMMATRIX _proj_inv = XMMatrixInverse(&XMMatrixDeterminant(_proj), _proj);
        XMMATRIX _vp = XMMatrixMultiply(_view, _proj);
        XMMATRIX _vp_inv = XMMatrixInverse(&XMMatrixDeterminant(_vp), _vp);

        // if we use mul(mat, vec) (instead of mul(vec, mat)) in hlsl
        // we don't need to write XMMatrixTranspose here
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

    void BuildDescriporHeaps() {
        UINT n_obj = opaque_items.size();
        // n_obj obj_cbv & 1 pass_cbv per frame resource
        //         0 ~     n_obj - 1 - obj_cb for frame resource 1
        //     n_obj ~ 2 * n_obj - 1 - obj_cb for frame resource 2
        // 2 * n_obj ~ 3 * n_obj - 1 - obj_cb for frame resource 3
        // 3 * n_obj ~ 3 * n_obj + 2 - pass_cb for each frame resource
        UINT n_descriptor = (n_obj + 1) * n_frame_resource;
        pass_cbv_offset = n_obj * n_frame_resource;

        D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc;
        cbv_heap_desc.NumDescriptors = n_descriptor;
        cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        cbv_heap_desc.NodeMask = 0;
        ThrowIfFailed(p_device->CreateDescriptorHeap(&cbv_heap_desc, IID_PPV_ARGS(&p_cbv_heap)));
    }
    void BuildCBV() {
        UINT obj_cb_size = D3DUtil::CBSize(sizeof(ObjectConst));
        int n_obj = opaque_items.size();

        // per object cbv
        for (int fr_ind = 0; fr_ind < n_frame_resource; fr_ind++) {
            auto obj_cb = frame_resources[fr_ind]->p_obj_cb->Resource();
            for (int i = 0; i < n_obj; i++) {
                D3D12_GPU_VIRTUAL_ADDRESS cb_addr = obj_cb->GetGPUVirtualAddress();
                cb_addr += i * obj_cb_size;
                int heap_ind = fr_ind * n_obj + i;
                auto h_obj_cbv = CD3DX12_CPU_DESCRIPTOR_HANDLE(p_cbv_heap->GetCPUDescriptorHandleForHeapStart(),
                    heap_ind, cbv_srv_uav_descriptor_size);
                D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
                cbv_desc.SizeInBytes = obj_cb_size;
                cbv_desc.BufferLocation = cb_addr;
                p_device->CreateConstantBufferView(&cbv_desc, h_obj_cbv);
            }
        }

        // per pass cbv
        UINT pass_cb_size = D3DUtil::CBSize(sizeof(PassConst));
        for (int fr_ind = 0; fr_ind < n_frame_resource; fr_ind++) {
            auto pass_cb = frame_resources[fr_ind]->p_pass_cb->Resource();
            D3D12_GPU_VIRTUAL_ADDRESS cb_addr = pass_cb->GetGPUVirtualAddress();
            auto h_pass_cbv = CD3DX12_CPU_DESCRIPTOR_HANDLE(p_cbv_heap->GetCPUDescriptorHandleForHeapStart(),
                pass_cbv_offset + fr_ind, cbv_srv_uav_descriptor_size);
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
            cbv_desc.SizeInBytes = pass_cb_size;
            cbv_desc.BufferLocation = cb_addr;
            p_device->CreateConstantBufferView(&cbv_desc, h_pass_cbv);
        }
    }
    void BuildRootSignature() {
        CD3DX12_DESCRIPTOR_RANGE cbv_table0; // per obj - b0
        cbv_table0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        CD3DX12_DESCRIPTOR_RANGE cbv_table1; // per pass - b1
        cbv_table1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);
        // use update frequency to group data in cbuufer

        CD3DX12_ROOT_PARAMETER rt_params[2];
        rt_params[0].InitAsDescriptorTable(1, &cbv_table0);
        rt_params[1].InitAsDescriptorTable(1, &cbv_table1);

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
        shaders["standard_vs"] = D3DUtil::CompileShader(src_path + L"ch07_shape/shaders/shape.hlsl",
            nullptr, "VS", "vs_5_1");
        shaders["opaque_ps"] = D3DUtil::CompileShader(src_path + L"ch07_shape/shaders/shape.hlsl",
            nullptr, "PS", "ps_5_1");

        // input layout and input elements specify input of (vertex) shader
        input_layout = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
    }
    void BuildShapeGeometries() {
        GeometryGenerator geo_gen;
        GeometryGenerator::MeshData box = geo_gen.Box(1.5f, 0.5f, 1.5f, 3);
        GeometryGenerator::MeshData grid = geo_gen.Grid(20.0f, 30.0f, 60, 40);
        GeometryGenerator::MeshData sphere = geo_gen.Sphere(0.5f, 20, 20);
        GeometryGenerator::MeshData cylinder = geo_gen.Cylinder(0.5f, 0.3f, 3.0f, 20, 20);
        GeometryGenerator::MeshData geosphere = geo_gen.Geosphere(0.5f, 2);

        // concatenate all the geometry into one big vertex/index buffer

        // vertex offsets to each object
        UINT box_vertex_offset = 0;
        UINT grid_vertex_offset = box.vertices.size();
        UINT sphere_vertex_offset = grid_vertex_offset + grid.vertices.size();
        UINT cylinder_vertex_offset = sphere_vertex_offset + sphere.vertices.size();
        UINT geosphere_vertex_offset = cylinder_vertex_offset + cylinder.vertices.size();

        // starting index for each object
        UINT box_index_offset = 0;
        UINT grid_index_offset = box.indices32.size();
        UINT sphere_index_offset = grid_index_offset + grid.indices32.size();
        UINT cylinder_index_offset = sphere_index_offset + sphere.indices32.size();
        UINT geosphere_index_offset = cylinder_index_offset + cylinder.indices32.size();

        // define submeshes
        SubmeshGeometry box_submesh;
        box_submesh.n_index = box.indices32.size();
        box_submesh.start_index = box_index_offset;
        box_submesh.base_vertex = box_vertex_offset;

        SubmeshGeometry grid_submesh;
        grid_submesh.n_index = grid.indices32.size();
        grid_submesh.start_index = grid_index_offset;
        grid_submesh.base_vertex = grid_vertex_offset;

        SubmeshGeometry sphere_submesh;
        sphere_submesh.n_index = sphere.indices32.size();
        sphere_submesh.start_index = sphere_index_offset;
        sphere_submesh.base_vertex = sphere_vertex_offset;

        SubmeshGeometry cylinder_submesh;
        cylinder_submesh.n_index = cylinder.indices32.size();
        cylinder_submesh.start_index = cylinder_index_offset;
        cylinder_submesh.base_vertex = cylinder_vertex_offset;

        SubmeshGeometry geosphere_submesh;
        geosphere_submesh.n_index = geosphere.indices32.size();
        geosphere_submesh.start_index = geosphere_index_offset;
        geosphere_submesh.base_vertex = geosphere_vertex_offset;

        // extract the vertex elements we are interested in and pack the
        // vertices of all the meshes into one vertex buffer.
        auto total_vertex_cnt = box.vertices.size() + grid.vertices.size() +
            sphere.vertices.size() + cylinder.vertices.size() + geosphere.vertices.size();
        std::vector<Vertex> vertices(total_vertex_cnt);
        UINT k = 0;
        for (int i = 0; i < box.vertices.size(); i++, k++) {
            vertices[k].pos = box.vertices[i].pos;
            vertices[k].color = XMFLOAT4(DirectX::Colors::DarkGreen);
        }
        for (int i = 0; i < grid.vertices.size(); i++, k++) {
            vertices[k].pos = grid.vertices[i].pos;
            vertices[k].color = XMFLOAT4(DirectX::Colors::ForestGreen);
        }
        for (int i = 0; i < sphere.vertices.size(); i++, k++) {
            vertices[k].pos = sphere.vertices[i].pos;
            vertices[k].color = XMFLOAT4(DirectX::Colors::Crimson);
        }
        for (int i = 0; i < cylinder.vertices.size(); i++, k++) {
            vertices[k].pos = cylinder.vertices[i].pos;
            vertices[k].color = XMFLOAT4(DirectX::Colors::SteelBlue);
        }
        for (int i = 0; i < geosphere.vertices.size(); i++, k++) {
            vertices[k].pos = geosphere.vertices[i].pos;
            vertices[k].color = XMFLOAT4(DirectX::Colors::DeepPink);
        }

        std::vector<uint16_t> indices;
        indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
        indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
        indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
        indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
        indices.insert(indices.end(), std::begin(geosphere.GetIndices16()), std::end(geosphere.GetIndices16()));

        const UINT vb_size = vertices.size() * sizeof(Vertex);
        const UINT ib_size = indices.size()  * sizeof(uint16_t);

        auto geo = std::make_unique<MeshGeometry>();
        geo->name = "shape_geo";

        ThrowIfFailed(D3DCreateBlob(vb_size, &geo->vb_cpu));
        CopyMemory(geo->vb_cpu->GetBufferPointer(), vertices.data(), vb_size);

        ThrowIfFailed(D3DCreateBlob(ib_size, &geo->ib_cpu));
        CopyMemory(geo->ib_cpu->GetBufferPointer(), indices.data(), ib_size);

        geo->vb_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(),
            p_cmd_list.Get(), vertices.data(), vb_size, geo->vb_uploader);

        geo->ib_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(),
            p_cmd_list.Get(), indices.data(), ib_size, geo->ib_uploader);

        geo->vb_stride = sizeof(Vertex);
        geo->vb_size = vb_size;
        geo->index_fmt = DXGI_FORMAT_R16_UINT;
        geo->ib_size = ib_size;

        geo->draw_args["box"] = box_submesh;
        geo->draw_args["grid"] = grid_submesh;
        geo->draw_args["sphere"] = sphere_submesh;
        geo->draw_args["cylinder"] = cylinder_submesh;
        geo->draw_args["geosphere"] = geosphere_submesh;

        geometries[geo->name] = std::move(geo);
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
            frame_resources.push_back(std::make_unique<FrameResource>(p_device.Get(), 1, items.size()));
        }
    }
    void BuildRenderItems() {
        UINT obj_cb_ind = 0;

        auto box_item = std::make_unique<RenderItem>();
        XMStoreFloat4x4(&box_item->model,
            // XMMATRIX::operator* and XMMatrixMultiply are equivalent
            XMMatrixScaling(2.0f, 2.0f, 2.0f) * XMMatrixTranslation(0.0f, 0.5f, 0.0f));
        box_item->obj_cb_ind = obj_cb_ind++;
        box_item->geo = geometries["shape_geo"].get();
        box_item->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        box_item->n_index = box_item->geo->draw_args["box"].n_index;
        box_item->start_index = box_item->geo->draw_args["box"].start_index;
        box_item->base_vertex = box_item->geo->draw_args["box"].base_vertex;
        items.emplace_back(std::move(box_item));

        auto grid_item = std::make_unique<RenderItem>();
        grid_item->model = DXMath::Identity4x4();
        grid_item->obj_cb_ind = obj_cb_ind++;
        grid_item->geo = geometries["shape_geo"].get();
        grid_item->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        grid_item->n_index = grid_item->geo->draw_args["grid"].n_index;
        grid_item->start_index = grid_item->geo->draw_args["grid"].start_index;
        grid_item->base_vertex = grid_item->geo->draw_args["grid"].base_vertex;
        items.emplace_back(std::move(grid_item));

        for (int i = 0; i < 5; i++) {
            auto left_cylinder_item = std::make_unique<RenderItem>();
            auto right_cylinder_item = std::make_unique<RenderItem>();
            auto left_sphere_item = std::make_unique<RenderItem>();
            auto right_sphere_item = std::make_unique<RenderItem>();

            XMMATRIX left_cylinder_model = XMMatrixTranslation(-5.0f, 1.5f, -10.0f + i * 5.0f);
            XMMATRIX right_cylinder_model = XMMatrixTranslation(5.0f, 1.5f, -10.0f + i * 5.0f);

            XMMATRIX left_sphere_model = XMMatrixTranslation(-5.0f, 3.5f, -10.0f + i * 5.0f);
            XMMATRIX right_sphere_model = XMMatrixTranslation(5.0f, 3.5f, -10.0f + i * 5.0f);

            XMStoreFloat4x4(&left_cylinder_item->model, right_cylinder_model);
            left_cylinder_item->obj_cb_ind = obj_cb_ind++;
            left_cylinder_item->geo = geometries["shape_geo"].get();
            left_cylinder_item->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            left_cylinder_item->n_index = left_cylinder_item->geo->draw_args["cylinder"].n_index;
            left_cylinder_item->start_index = left_cylinder_item->geo->draw_args["cylinder"].start_index;
            left_cylinder_item->base_vertex = left_cylinder_item->geo->draw_args["cylinder"].base_vertex;

            XMStoreFloat4x4(&right_cylinder_item->model, left_cylinder_model);
            right_cylinder_item->obj_cb_ind = obj_cb_ind++;
            right_cylinder_item->geo = geometries["shape_geo"].get();
            right_cylinder_item->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            right_cylinder_item->n_index = right_cylinder_item->geo->draw_args["cylinder"].n_index;
            right_cylinder_item->start_index = right_cylinder_item->geo->draw_args["cylinder"].start_index;
            right_cylinder_item->base_vertex = right_cylinder_item->geo->draw_args["cylinder"].base_vertex;

            XMStoreFloat4x4(&left_sphere_item->model, left_sphere_model);
            left_sphere_item->obj_cb_ind = obj_cb_ind++;
            left_sphere_item->geo = geometries["shape_geo"].get();
            left_sphere_item->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            left_sphere_item->n_index = left_sphere_item->geo->draw_args["sphere"].n_index;
            left_sphere_item->start_index = left_sphere_item->geo->draw_args["sphere"].start_index;
            left_sphere_item->base_vertex = left_sphere_item->geo->draw_args["sphere"].base_vertex;

            XMStoreFloat4x4(&right_sphere_item->model, right_sphere_model);
            right_sphere_item->obj_cb_ind = obj_cb_ind++;
            right_sphere_item->geo = geometries["shape_geo"].get();
            right_sphere_item->prim_ty = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            right_sphere_item->n_index = right_sphere_item->geo->draw_args["geosphere"].n_index;
            right_sphere_item->start_index = right_sphere_item->geo->draw_args["geosphere"].start_index;
            right_sphere_item->base_vertex = right_sphere_item->geo->draw_args["geosphere"].base_vertex;

            items.emplace_back(std::move(left_cylinder_item));
            items.emplace_back(std::move(right_cylinder_item));
            items.emplace_back(std::move(left_sphere_item));
            items.emplace_back(std::move(right_sphere_item));
        }

        for (const auto &item : items) {
            opaque_items.push_back(item.get());
        }
    }

    void DrawRenderItems(ID3D12GraphicsCommandList *cmd_list, const std::vector<RenderItem *> &items) {
        UINT obj_cb_size = D3DUtil::CBSize(sizeof(ObjectConst));
        auto obj_cb = curr_fr->p_obj_cb.get();
        for (auto item : items) { // per object
            // set vb, ib and primitive type
            cmd_list->IASetVertexBuffers(0, 1, &item->geo->VertexBufferView());
            cmd_list->IASetIndexBuffer(&item->geo->IndexBufferView());
            cmd_list->IASetPrimitiveTopology(item->prim_ty);

            // set per object cbv
            UINT cbv_ind = curr_fr_ind * opaque_items.size() + item->obj_cb_ind;
            auto h_obj_cbv = CD3DX12_GPU_DESCRIPTOR_HANDLE(p_cbv_heap->GetGPUDescriptorHandleForHeapStart(),
                cbv_ind, cbv_srv_uav_descriptor_size);
            cmd_list->SetGraphicsRootDescriptorTable(0, h_obj_cbv);

            // draw
            cmd_list->DrawIndexedInstanced(item->n_index, 1, item->start_index, item->base_vertex, 0);
        }
    }
    
    std::vector<std::unique_ptr<FrameResource>> frame_resources;
    FrameResource *curr_fr = nullptr;
    int curr_fr_ind = 0;

    ComPtr<ID3D12RootSignature> p_rt_sig = nullptr;
    ComPtr<ID3D12DescriptorHeap> p_cbv_heap = nullptr;

    std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> geometries;
    std::unordered_map<std::string, ComPtr<ID3DBlob>> shaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> psos;

    std::vector<D3D12_INPUT_ELEMENT_DESC> input_layout;

    std::vector<std::unique_ptr<RenderItem>> items;
    std::vector<RenderItem *> opaque_items;

    PassConst main_pass_cb;
    UINT pass_cbv_offset = 0;

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
        D3DAppShape d3d_app(h_inst);
        if (!d3d_app.Initialize()) {
            return 0;
        }
        return d3d_app.Run();
    } catch (DxException &e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}