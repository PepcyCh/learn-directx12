#include "D3DApp.h"

#include <cassert>
#include <vector>

#include <windowsx.h>

#include "d3dx12.h"
#include "D3DUtil.h"

using Microsoft::WRL::ComPtr;

LRESULT CALLBACK
MainWndProc(HWND win, UINT msg, WPARAM w_param, LPARAM l_param) {
    return D3DApp::GetApp()->MsgProc(win, msg, w_param, l_param);
}

D3DApp::D3DApp(HINSTANCE h_inst) : h_appinst(h_inst) {
    assert(g_app == nullptr);
    g_app = this;
}

D3DApp::~D3DApp() {
    if (p_device != nullptr) {
        FlushCommandQueue();
    }
}

D3DApp *D3DApp::GetApp() {
    return g_app;
}

HINSTANCE D3DApp::AppInst() const {
    return h_appinst;
}

HWND D3DApp::Window() const {
    return h_win;
}

float D3DApp::Aspect() const {
    return float(client_width) / client_height;
}

int D3DApp::Run() {
    MSG msg = {};
    timer.Reset();

    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        } else {
            timer.Tick();
            if (!paused) {
                CalcFrameStats();
                Update(timer);
                Draw(timer);
            } else {
                Sleep(100);
            }
        }
    }

    return msg.wParam;
}

LRESULT D3DApp::MsgProc(HWND win, UINT msg, WPARAM w_param, LPARAM l_param) {
    switch (msg) {
        case WM_ACTIVATE: {
            if (LOWORD(w_param) == WA_ACTIVE) {
                paused = true;
                timer.Stop();
            } else {
                paused = false;
                timer.Start();
            }
            return 0;
        }
        case WM_SIZE: {
            client_width = LOWORD(l_param);
            client_height = HIWORD(l_param);
            if (p_device) {
                if (w_param == SIZE_MINIMIZED) {
                    minimized = true;
                    maximized = false;
                    paused = true;
                } else if (w_param == SIZE_MAXIMIZED) {
                    minimized = false;
                    maximized = true;
                    paused = false;
                    OnResize();
                } else if (w_param == SIZE_RESTORED) {
                    if (minimized) {
                        paused = false;
                        minimized = false;
                        OnResize();
                    } else if (maximized) {
                        paused = false;
                        maximized = false;
                        OnResize();
                    } else if (resizing) {
                        ;
                    } else {
                        OnResize();
                    }
                }
            }
            return 0;
        }
        case WM_ENTERSIZEMOVE: {
            paused = true;
            resizing = true;
            timer.Stop();
            return 0;
        }
        case WM_EXITSIZEMOVE: {
            paused = false;
            resizing = false;
            timer.Start();
            OnResize();
            return 0;
        }
        case WM_DESTROY: {
            PostQuitMessage(0);
            return 0;
        }
        case WM_MENUCHAR: {
            return MAKELRESULT(0, MNC_CLOSE);
        }
        case WM_GETMINMAXINFO: {
            ((MINMAXINFO *) l_param)->ptMinTrackSize.x = 200;
            ((MINMAXINFO *) l_param)->ptMinTrackSize.y = 200;
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
        case WM_RBUTTONDOWN: {
            OnMouseDown(w_param, GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
            return 0;
        }
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_RBUTTONUP: {
            OnMouseUp(w_param, GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
            return 0;
        }
        case WM_MOUSEMOVE: {
            OnMouseMove(w_param, GET_X_LPARAM(l_param), GET_Y_LPARAM(l_param));
            return 0;
        }
        case WM_KEYUP: {
            if (w_param == VK_ESCAPE) {
                PostQuitMessage(0);
            }
            return 0;
        }
    }
    return DefWindowProc(win, msg, w_param, l_param);
}

bool D3DApp::Initialize() {
    if (!InitWindow()) {
        return false;
    }
    if (!InitDirect3D()) {
        return false;
    }

    OnResize();

    return true;
}

bool D3DApp::InitWindow() {
    WNDCLASS wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = MainWndProc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = h_appinst;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH) GetStockObject(NULL_BRUSH);
    wc.lpszMenuName = nullptr;
    wc.lpszClassName = L"Main Window";

    if (!RegisterClass(&wc)) {
        MessageBox(nullptr, L"RegisterClass failed", nullptr, 0);
        return false;
    }

    RECT rect { 0, 0, client_width, client_height };
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, false);
    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    h_win = CreateWindow(L"Main Window", win_caption.c_str(),
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, h_appinst, nullptr);
    if (!h_win) {
        DWORD errc = GetLastError();
        MessageBox(nullptr, (L"CreateWindow failed, err code: " + std::to_wstring(errc)).c_str(),
            nullptr, 0);
        return false;
    }

    ShowWindow(h_win, SW_SHOW);
    UpdateWindow(h_win);

    return true;
}

bool D3DApp::InitDirect3D() {
    // debug layer
#if defined(DEBUG) || defined(_DEBUG)
    ComPtr<ID3D12Debug> p_dbg_controller;
    ThrowIfFailed(D3D12GetDebugInterface(IID_PPV_ARGS(&p_dbg_controller)));
    p_dbg_controller->EnableDebugLayer();
#endif

    // create factory
    ThrowIfFailed(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&p_dxgi_factory)));

    // create device
    D3D_FEATURE_LEVEL d3d_feature = D3D_FEATURE_LEVEL_12_0;
    HRESULT hr_hardware = D3D12CreateDevice(nullptr, d3d_feature, IID_PPV_ARGS(&p_device));
    if (FAILED(hr_hardware)) {
        ComPtr<IDXGIAdapter> p_warp_adapter;
        ThrowIfFailed(p_dxgi_factory->EnumWarpAdapter(IID_PPV_ARGS(&p_warp_adapter)));
        ThrowIfFailed(D3D12CreateDevice(p_warp_adapter.Get(), d3d_feature, IID_PPV_ARGS(&p_device)));
        MessageBox(nullptr, L"warp adapter", nullptr, 0);
    }

    // create fence
    ThrowIfFailed(p_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p_fence)));

    // get descriptor size
    rtv_descriptor_size = p_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    dsv_descriptor_size = p_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    cbv_srv_uav_descriptor_size = p_device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

#if defined(DEBUG) || defined(_DEBUG)
    LogAdapters();
#endif
    
    CreateCommandObjects();
    CreateSwapChain();
    CreateDescriptorHeaps();

    return true;
}

void D3DApp::CreateCommandObjects() {
    D3D12_COMMAND_LIST_TYPE cmd_list_ty = D3D12_COMMAND_LIST_TYPE_DIRECT;

    D3D12_COMMAND_QUEUE_DESC cmd_queue_desc = {};
    cmd_queue_desc.Type = cmd_list_ty;
    cmd_queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(p_device->CreateCommandQueue(&cmd_queue_desc, IID_PPV_ARGS(&p_cmd_queue)));

    ThrowIfFailed(p_device->CreateCommandAllocator(cmd_list_ty, IID_PPV_ARGS(&p_cmd_allocator)));

    ThrowIfFailed(p_device->CreateCommandList(0, cmd_list_ty,
        p_cmd_allocator.Get(), nullptr, IID_PPV_ARGS(&p_cmd_list)));

    p_cmd_list->Close();
}

void D3DApp::CreateSwapChain() {
    p_swap_chain.Reset();

    // different from d3d12book
    DXGI_SWAP_CHAIN_DESC1 sc_desc = {};
    sc_desc.Width = client_width;
    sc_desc.Height = client_height;
    sc_desc.Format = back_buffer_fmt;
    sc_desc.SampleDesc.Count = 1;
    sc_desc.SampleDesc.Quality = 0;
    sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sc_desc.BufferCount = kSwapChainBufferCnt;
    sc_desc.Scaling = DXGI_SCALING_STRETCH;
    sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sc_desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC sc_fs_desc = {};
    sc_fs_desc.Windowed = true;

    ThrowIfFailed(p_dxgi_factory->CreateSwapChainForHwnd(p_cmd_queue.Get(), h_win,
        &sc_desc, &sc_fs_desc, nullptr, &p_swap_chain));
}

void D3DApp::CreateDescriptorHeaps() {
    // rtv
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = kSwapChainBufferCnt;
    rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_heap_desc.NodeMask = 0;
    ThrowIfFailed(p_device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&p_rtv_heap)));
    // dsv
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc;
    dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_desc.NumDescriptors = 1;
    dsv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_heap_desc.NodeMask = 0;
    ThrowIfFailed(p_device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&p_dsv_heap)));
}

void D3DApp::OnResize() {
    assert(p_device && "null p_device in OnResize");
    assert(p_swap_chain && "null p_swap_chain in OnResize");
    assert(p_cmd_allocator && "null p_cmd_allocator in OnResize");
    // flush & reset
    FlushCommandQueue();

    ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), nullptr));
    for (int i = 0; i < kSwapChainBufferCnt; i++) {
        swap_chain_buffers[i].Reset();
    }
    depth_stencil_buffer.Reset();

    // resize swap chain
    ThrowIfFailed(p_swap_chain->ResizeBuffers(kSwapChainBufferCnt,
        client_width, client_height, back_buffer_fmt,
        DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));
    curr_back_buffer = 0;
    // create rtv
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtv_heap_handle(p_rtv_heap->GetCPUDescriptorHandleForHeapStart());
    for (int i = 0; i < kSwapChainBufferCnt; i++) {
        ThrowIfFailed(p_swap_chain->GetBuffer(i, IID_PPV_ARGS(&swap_chain_buffers[i])));
        p_device->CreateRenderTargetView(swap_chain_buffers[i].Get(), nullptr, rtv_heap_handle);
        rtv_heap_handle.Offset(1, rtv_descriptor_size);
    }

    // create depth/stencil buffer resource
    D3D12_RESOURCE_DESC depth_stencil_desc;
    depth_stencil_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depth_stencil_desc.Alignment = 0;
    depth_stencil_desc.Width = client_width;
    depth_stencil_desc.Height = client_height;
    depth_stencil_desc.DepthOrArraySize = 1;
    depth_stencil_desc.MipLevels = 1;
    // depth_stencil_desc.Format = depth_stencil_fmt;
    depth_stencil_desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
    depth_stencil_desc.SampleDesc.Count = 1;
    depth_stencil_desc.SampleDesc.Quality = 0;
    depth_stencil_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depth_stencil_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_CLEAR_VALUE opt_clear;
    opt_clear.Format = depth_stencil_fmt;
    opt_clear.DepthStencil.Depth = 1.0f;
    opt_clear.DepthStencil.Stencil = 0;
    ThrowIfFailed(p_device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE, &depth_stencil_desc, D3D12_RESOURCE_STATE_COMMON,
        &opt_clear, IID_PPV_ARGS(&depth_stencil_buffer)));
    // create dsv
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
    dsv_desc.Format = depth_stencil_fmt;
    dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsv_desc.Texture2D.MipSlice = 0;
    dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
    p_device->CreateDepthStencilView(depth_stencil_buffer.Get(), &dsv_desc, DepthStencilView());
    // ds recourse state transit
    p_cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(depth_stencil_buffer.Get(), 
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));
    
    // execute & flush
    ThrowIfFailed(p_cmd_list->Close());
    ID3D12CommandList *cmds[] = { p_cmd_list.Get() };
    p_cmd_queue->ExecuteCommandLists(sizeof(cmds) / sizeof(cmds[0]), cmds);
    FlushCommandQueue();

    // viewport & scissors
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = client_width;
    viewport.Height = client_height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    scissors = { 0, 0, client_width, client_height };
}

void D3DApp::FlushCommandQueue() {
    ++curr_fence;
    ThrowIfFailed(p_cmd_queue->Signal(p_fence.Get(), curr_fence));
    if (p_fence->GetCompletedValue() < curr_fence) {
        HANDLE event = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(p_fence->SetEventOnCompletion(curr_fence, event));
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);
    }
}

void D3DApp::CalcFrameStats() {
    static int frame_cnt = 0;
    static double time_elapsed = 0.0;

    ++frame_cnt;
    if (timer.TotalTime() - time_elapsed >= 1.0) {
        double fps = frame_cnt;
        double mspf = 1000.0 / frame_cnt;
        std::wstring fps_str = std::to_wstring(fps);
        std::wstring mspf_str = std::to_wstring(mspf);
        std::wstring text = win_caption + L"  fps:" + fps_str + L"  mspf:" + mspf_str;
        SetWindowText(h_win, text.c_str());
        frame_cnt = 0;
        time_elapsed += 1.0;
    }
}

ID3D12Resource *D3DApp::CurrBackBuffer() const {
    return swap_chain_buffers[curr_back_buffer].Get();
}
D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::CurrBackBufferView() const {
    return CD3DX12_CPU_DESCRIPTOR_HANDLE(
        p_rtv_heap->GetCPUDescriptorHandleForHeapStart(), curr_back_buffer, rtv_descriptor_size);
}
D3D12_CPU_DESCRIPTOR_HANDLE D3DApp::DepthStencilView() const {
    return p_dsv_heap->GetCPUDescriptorHandleForHeapStart();
}

void D3DApp::LogAdapters() {
    UINT i = 0;
    IDXGIAdapter *adapter = nullptr;
    std::vector<IDXGIAdapter *> adapters;
    while (p_dxgi_factory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC desc;
        adapter->GetDesc(&desc);
        std::wstring text = L"***Adapter: ";
        text += desc.Description;
        text += L"\n";
        OutputDebugString(text.c_str());
        adapters.push_back(adapter);
        ++i;
    }

    for (auto &adapter : adapters) {
        LogAdapterOutputs(adapter);
        adapter->Release();
        adapter = nullptr;
    }
}

void D3DApp::LogAdapterOutputs(IDXGIAdapter *adapter) {
    UINT i = 0;
    IDXGIOutput *output = nullptr;
    while (adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND) {
        DXGI_OUTPUT_DESC desc;
        output->GetDesc(&desc);
        std::wstring text = L"***Output: ";
        text += desc.DeviceName;
        text += L"\n";
        OutputDebugString(text.c_str());
        LogOutputDisplayModes(output, back_buffer_fmt);
        output->Release();
        output = nullptr;
        ++i;
    }
}

void D3DApp::LogOutputDisplayModes(IDXGIOutput *output, DXGI_FORMAT fmt) {
    UINT count = 0;
    UINT flags = 0;

    output->GetDisplayModeList(fmt, flags, &count, nullptr);
    std::vector<DXGI_MODE_DESC> modes(count);
    output->GetDisplayModeList(fmt, flags, &count, &modes[0]);

    for (auto &mode : modes) {
        UINT n = mode.RefreshRate.Numerator;
        UINT d = mode.RefreshRate.Denominator;
        std::wstring text =
            L"Width = " + std::to_wstring(mode.Width) + L" " +
            L"Height = " + std::to_wstring(mode.Height) + L" " +
            L"Refresh = " + std::to_wstring(n) + L"/" + std::to_wstring(d) +
            L"\n";

        OutputDebugString(text.c_str());
    }
}