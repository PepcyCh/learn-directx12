#include <algorithm>
#include <array>
#include <memory>

#include <d3dcompiler.h>
#include <DirectXColors.h>

#include "../defines.h"
#include "MeshGeometryEx.h"
#include "D3DApp.h"
#include "D3DUtil.h"
#include "DXMath.h"
#include "UploadBuffer.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// objects in cbuffer
struct CBObject {
    XMFLOAT4X4 mvp_mat = DXMath::Identity4x4();
};

// this app is an extention of chap.6 code (ch06_box)
// 1. use RH matrices, therefore:
//    front face is ccw (in BuildPSO())
//    OnMouseMove() is modified (theta += dx -> theta -= dx)
// 2. use 2 vertex buffers, therefore:
//    MeshGeomtry needs to be changed -> MeshGeometryEx
//    Draw() and BuildBoxGeometry() are changed

class D3DAppBoxExtra : public D3DApp {
  public:
    D3DAppBoxExtra(HINSTANCE h_inst) : D3DApp(h_inst) {}
    ~D3DAppBoxExtra() {}

    bool Initialize() override {
        if (!D3DApp::Initialize()) {
            return false;
        }

        ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), nullptr));

        BuildDescriptorHeaps();
        BuildConstantBuffers();
        BuildRootSignature();
        BuildShadersAndInputLayout();
        BuildBoxGeometry();
        BuildPSO();

        ThrowIfFailed(p_cmd_list->Close());
        ID3D12CommandList *cmds[] = { p_cmd_list.Get() };
        p_cmd_queue->ExecuteCommandLists(sizeof(cmds) / sizeof(cmds[0]), cmds);
        FlushCommandQueue();

        return true;
    }

  private:
    void OnResize() override {
        D3DApp::OnResize();
        XMMATRIX p = XMMatrixPerspectiveFovRH(XM_PIDIV4, Aspect(), 0.1f, 100.0f);
        XMStoreFloat4x4(&project, p);
    }

    void Update(const Timer &timer) override {
        // camera moves on a circle
        float x = radius * std::sin(phi) * std::cos(theta);
        float y = radius * std::cos(phi);
        float z = radius * std::sin(phi) * std::sin(theta);

        XMVECTOR pos = XMVectorSet(x, y, z, 1.0f);
        XMVECTOR lookat = XMVectorZero();
        XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        XMMATRIX _view = XMMatrixLookAtRH(pos, lookat, up); // RH is for right-hand
        XMStoreFloat4x4(&view, _view);

        XMMATRIX _model = XMLoadFloat4x4(&model);
        XMMATRIX _project = XMLoadFloat4x4(&project);
        XMMATRIX mvp_mat = _model * _view * _project;
        CBObject obj_const;
        // XMMATRIX / XMFLOAT4X4 is row-majar, but hlsl matrix is column-major
        // therefore XMMatrixTranspose is needed
        XMStoreFloat4x4(&obj_const.mvp_mat, XMMatrixTranspose(mvp_mat));

        p_cbobj->CopyData(0, obj_const);
    }

    void Draw(const Timer &timer) override {
        // modified to support multiple vertex buffers

        // reset
        ThrowIfFailed(p_cmd_allocator->Reset());
        ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), p_pso.Get())); // set pso

        // viewport & scissor
        p_cmd_list->RSSetViewports(1, &viewport);
        p_cmd_list->RSSetScissorRects(1, &scissors);

        // back buffer: present -> render target
        p_cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));
        // clear rtv and dsv
        p_cmd_list->ClearRenderTargetView(CurrBackBufferView(), Colors::LightBlue, 0, nullptr);
        p_cmd_list->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL,
            1.0f, 0, 0, nullptr);
        // set rtv and dsv
        p_cmd_list->OMSetRenderTargets(1, &CurrBackBufferView(), true, &DepthStencilView());

        // set heaps (cbv_srv_uav)
        ID3D12DescriptorHeap *heaps[] = { p_cbv_heap.Get() };
        p_cmd_list->SetDescriptorHeaps(sizeof(heaps) / sizeof(heaps[0]), heaps);

        // root signature
        p_cmd_list->SetGraphicsRootSignature(p_rt_sig.Get());

        // set vbv, ibv and primitive topology
        // pay attention to 'D3D12_PRIMITIVE_TOPOLOGY' and 'D3D12_PRIMITIVE_TOPOLOGY_TYPE'
        // more than 1 vbv
        D3D12_VERTEX_BUFFER_VIEW vbvs[] = { p_mesh->VertexBufferView(0), p_mesh->VertexBufferView(1) };
        p_cmd_list->IASetVertexBuffers(0, sizeof(vbvs) / sizeof(vbvs[0]), vbvs);
        p_cmd_list->IASetIndexBuffer(&p_mesh->IndexBufferView());
        p_cmd_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // set root descriptor table
        p_cmd_list->SetGraphicsRootDescriptorTable(0, p_cbv_heap->GetGPUDescriptorHandleForHeapStart());

        // draw vertices with indices
        p_cmd_list->DrawIndexedInstanced(p_mesh->draw_args["box"].n_index, 1, 0, 0, 0);

        // back buffer: render target -> present
        p_cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

        // close, swap and flush
        ThrowIfFailed(p_cmd_list->Close());
        ID3D12CommandList *cmds[] = { p_cmd_list.Get() };
        p_cmd_queue->ExecuteCommandLists(sizeof(cmds) / sizeof(cmds[0]), cmds);
        ThrowIfFailed(p_swap_chain->Present(0, 0));
        curr_back_buffer = (curr_back_buffer + 1) % kSwapChainBufferCnt;
        FlushCommandQueue();
    }

    void OnMouseDown(WPARAM btn_state, int x, int y) override {
        last_mouse.x = x;
        last_mouse.y = y;
        SetCapture(h_win);
    }

    void OnMouseUp(WPARAM btn_state, int x, int y) override {
        ReleaseCapture();
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

    void BuildDescriptorHeaps() {
        // create cbv heap
        D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_desc;
        cbv_heap_desc.NumDescriptors = 1;
        cbv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // shader visible !
        cbv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        cbv_heap_desc.NodeMask = 0;
        p_device->CreateDescriptorHeap(&cbv_heap_desc, IID_PPV_ARGS(&p_cbv_heap));
    }

	void BuildConstantBuffers() {
        // create upload buffer for cb object
        p_cbobj = std::make_unique<UploadBuffer<CBObject>>(p_device.Get(), 1, true);
        // create cbv in cbv heap
        // 'bind' upload buffer's mapped resource and cbv's buffer location
        UINT cb_size = D3DUtil::CBSize(sizeof(CBObject));
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_desc;
        cbv_desc.BufferLocation = p_cbobj->Resource()->GetGPUVirtualAddress();
        cbv_desc.SizeInBytes = cb_size;
        p_device->CreateConstantBufferView(&cbv_desc, p_cbv_heap->GetCPUDescriptorHandleForHeapStart());
    }

    void BuildRootSignature() {
        // root signature is an array of root parameters
        // root parameter is either a descriptor table, root descriptor or root constant
        // root signature specifies cbuffers, textures, samplers that will be used in shader

        CD3DX12_ROOT_PARAMETER slot_rt_params[1];

        CD3DX12_DESCRIPTOR_RANGE cbv_table;
        // D3D12_DESCRIPTOR_RANGE_TYPE_CBV - 'b'
        // 1 - number
        // 0 - from '0'
        // -> register(b0)
        cbv_table.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);
        slot_rt_params[0].InitAsDescriptorTable(1, &cbv_table);

        CD3DX12_ROOT_SIGNATURE_DESC rt_sig_desc(1, slot_rt_params, 0, nullptr,
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        // serialze root signature to create
        ComPtr<ID3DBlob> serialized_rt_sig = nullptr;
        ComPtr<ID3DBlob> error = nullptr;
        HRESULT hr = D3D12SerializeRootSignature(&rt_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1,
            &serialized_rt_sig, &error);
        if (error != nullptr) {
            OutputDebugStringA((char *) error->GetBufferPointer());
        }
        ThrowIfFailed(hr);

        // create root signature
        ThrowIfFailed(p_device->CreateRootSignature(0, serialized_rt_sig->GetBufferPointer(),
            serialized_rt_sig->GetBufferSize(), IID_PPV_ARGS(&p_rt_sig)));
    }

    void BuildShadersAndInputLayout() {
        // build shader from hlsl file
        p_vs = D3DUtil::CompileShader(src_path + L"ch06_box/shaders/box.hlsl", nullptr, "VS", "vs_5_1");
        p_ps = D3DUtil::CompileShader(src_path + L"ch06_box/shaders/box.hlsl", nullptr, "PS", "ps_5_1");
        
        // input layout and input elements specify input of (vertex) shader
        input_layouts = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
    }

    void BuildBoxGeometry() {
        // modified to support multiple vertex buffers
        const int n_vertices = 8;
        const std::array<XMFLOAT3, n_vertices> poss = {
            XMFLOAT3(-1.0f, -1.0f, -1.0f),
            XMFLOAT3(-1.0f,  1.0f, -1.0f),
            XMFLOAT3( 1.0f,  1.0f, -1.0f),
            XMFLOAT3( 1.0f, -1.0f, -1.0f),
            XMFLOAT3(-1.0f, -1.0f,  1.0f),
            XMFLOAT3(-1.0f,  1.0f,  1.0f),
            XMFLOAT3( 1.0f,  1.0f,  1.0f),
            XMFLOAT3( 1.0f, -1.0f,  1.0f)
        };
        const std::array<XMFLOAT4, n_vertices> colors = {
            XMFLOAT4(Colors::Black),
            XMFLOAT4(Colors::Green),
            XMFLOAT4(Colors::Yellow),
            XMFLOAT4(Colors::Red),
            XMFLOAT4(Colors::Blue),
            XMFLOAT4(Colors::Cyan),
            XMFLOAT4(Colors::White),
            XMFLOAT4(Colors::Magenta)
        };
        const std::array<uint16_t, 36> indices = {
            // front face
            0, 1, 2, 0, 2, 3,
            // back face
            4, 6, 5, 4, 7, 6,
            // left face
            4, 5, 1, 4, 1, 0,
            // right face
            3, 2, 6, 3, 6, 7,
            // top face
            1, 5, 6, 1, 6, 2,
            // bottom face
            4, 0, 3, 4, 3, 7
        };

        const UINT vb_size[2] = { n_vertices * sizeof(XMFLOAT3), n_vertices * sizeof(XMFLOAT4) };
        const UINT ib_size = indices.size() * sizeof(uint16_t);

        p_mesh = std::make_unique<MeshGeometryEx>();
        p_mesh->name = "box_mesh";

        // create blob and copy data into it for vbs and ib
        ThrowIfFailed(D3DCreateBlob(vb_size[0], &p_mesh->vb_cpu[0]));
        CopyMemory(p_mesh->vb_cpu[0]->GetBufferPointer(), poss.data(), vb_size[0]);
        ThrowIfFailed(D3DCreateBlob(vb_size[1], &p_mesh->vb_cpu[1]));
        CopyMemory(p_mesh->vb_cpu[1]->GetBufferPointer(), colors.data(), vb_size[1]);
        ThrowIfFailed(D3DCreateBlob(ib_size, &p_mesh->ib_cpu));
        CopyMemory(p_mesh->ib_cpu->GetBufferPointer(), indices.data(), ib_size);

        // create buffer that holds the data for vbs and ib
        p_mesh->vb_gpu[0] = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            poss.data(), vb_size[0], p_mesh->vb_uploader[0]);
        p_mesh->vb_gpu[1] = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            colors.data(), vb_size[1], p_mesh->vb_uploader[1]);
        p_mesh->ib_gpu = D3DUtil::CreateDefaultBuffer(p_device.Get(), p_cmd_list.Get(),
            indices.data(), ib_size, p_mesh->ib_uploader);

        // other data of vbs and ib
        p_mesh->vb_stride[0] = sizeof(XMFLOAT3);
        p_mesh->vb_size[0] = vb_size[0];
        p_mesh->vb_stride[1] = sizeof(XMFLOAT4);
        p_mesh->vb_size[1] = vb_size[1];
        p_mesh->index_fmt = DXGI_FORMAT_R16_UINT;
        p_mesh->ib_size = ib_size;

        // submesh (a mesh can be a combination of some small submeshes)
        SubmeshGeometry submesh;
        submesh.n_index = indices.size();
        submesh.start_index = 0;
        submesh.base_vertex = 0;

        p_mesh->draw_args["box"] = submesh;
    }

    void BuildPSO() {
        // PSO(pipeline state object) specifies
        // * input layouts
        // * root signature
        // * shaders byte code
        // * blend
        // * rasterizer (cull face, fill mode ...)
        // * depth/stencil (depth func ...)
        // * sample
        // * # render targte, format of rtv and dsv
        // PSO can be considered as settings of RS state
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
        pso_desc.InputLayout = { input_layouts.data(), (UINT) input_layouts.size() };
        pso_desc.pRootSignature = p_rt_sig.Get();
        pso_desc.VS = {
            reinterpret_cast<BYTE *>(p_vs->GetBufferPointer()),
            p_vs->GetBufferSize()
        };
        pso_desc.PS = {
            reinterpret_cast<BYTE *>(p_ps->GetBufferPointer()),
            p_ps->GetBufferSize()
        };
        pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        pso_desc.RasterizerState.FrontCounterClockwise = true; // ccw front face
        pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.SampleDesc.Count = 1;
        pso_desc.SampleDesc.Quality = 0;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = back_buffer_fmt;
        pso_desc.DSVFormat = depth_stencil_fmt;
        ThrowIfFailed(p_device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&p_pso)));
    }

    ComPtr<ID3D12RootSignature> p_rt_sig;
    ComPtr<ID3D12DescriptorHeap> p_cbv_heap;

    std::unique_ptr<UploadBuffer<CBObject>> p_cbobj = nullptr;
    std::unique_ptr<MeshGeometryEx> p_mesh = nullptr;

    ComPtr<ID3DBlob> p_vs = nullptr;
    ComPtr<ID3DBlob> p_ps = nullptr;

    std::vector<D3D12_INPUT_ELEMENT_DESC> input_layouts;

    ComPtr<ID3D12PipelineState> p_pso = nullptr;

    XMFLOAT4X4 model = DXMath::Identity4x4();
    XMFLOAT4X4 view = DXMath::Identity4x4();
    XMFLOAT4X4 project = DXMath::Identity4x4();

    float theta = 0;
    float phi = XM_PIDIV2;
    float radius = 5.0f;

    POINT last_mouse;
};

int WINAPI WinMain(HINSTANCE h_inst, HINSTANCE h_prev_inst, PSTR cmd_line, int show_cmd) {
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try {
        D3DAppBoxExtra d3d_app(h_inst);
        if (!d3d_app.Initialize()) {
            return 0;
        }
        return d3d_app.Run();
    } catch (DxException &e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}