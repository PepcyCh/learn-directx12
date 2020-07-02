#pragma once

#include <string>

#if defined(DEBUG) || defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#include <windows.h>
#include <wrl.h>
#include <dxgi1_5.h>
#include <d3d12.h>

#include "Timer.h"

using namespace Microsoft;

class D3DApp {
  public:
    D3DApp(const D3DApp &rhs) = delete;
    D3DApp &operator=(const D3DApp &rhs) = delete;
    ~D3DApp();

    static D3DApp *GetApp();

    HINSTANCE AppInst() const;
    HWND Window() const;
    float Aspect() const;

    virtual bool Initialize();

    int Run();
    virtual LRESULT MsgProc(HWND win, UINT msg, WPARAM w_param, LPARAM l_param);

  protected:
    D3DApp(HINSTANCE h_inst);

    bool InitWindow();
    bool InitDirect3D();

    void CreateCommandObjects();
    void CreateSwapChain();
    virtual void CreateDescriptorHeaps();

    virtual void Update(const Timer &timer) = 0;
    virtual void Draw(const Timer &timer) = 0;

    virtual void OnResize();
    virtual void OnMouseUp(WPARAM btn_state, int x, int y) {}
    virtual void OnMouseMove(WPARAM btn_state, int x, int y) {}
    virtual void OnMouseDown(WPARAM btn_state, int x, int y) {}

    void FlushCommandQueue();
    void CalcFrameStats();

    ID3D12Resource *CurrBackBuffer() const;
    D3D12_CPU_DESCRIPTOR_HANDLE CurrBackBufferView() const;
    D3D12_CPU_DESCRIPTOR_HANDLE DepthStencilView() const;

    void LogAdapters();
    void LogAdapterOutputs(IDXGIAdapter *adapter);
    void LogOutputDisplayModes(IDXGIOutput *output, DXGI_FORMAT fmt);

    inline static D3DApp *g_app = nullptr;

    HINSTANCE h_appinst = nullptr;
    HWND h_win = nullptr;
    bool paused = false;
    bool minimized = false;
    bool maximized = false;
    bool resizing = false;
    bool full_screen_state = false;

    WRL::ComPtr<IDXGIFactory4> p_dxgi_factory;
    WRL::ComPtr<IDXGISwapChain1> p_swap_chain;
    WRL::ComPtr<ID3D12Device> p_device;

    WRL::ComPtr<ID3D12Fence> p_fence;
    UINT64 curr_fence;

    WRL::ComPtr<ID3D12CommandQueue> p_cmd_queue;
    WRL::ComPtr<ID3D12CommandAllocator> p_cmd_allocator;
    WRL::ComPtr<ID3D12GraphicsCommandList> p_cmd_list;

    static const int kSwapChainBufferCnt = 2;
    int curr_back_buffer = 0;
    WRL::ComPtr<ID3D12Resource> swap_chain_buffers[kSwapChainBufferCnt];
    WRL::ComPtr<ID3D12Resource> depth_stencil_buffer;
    DXGI_FORMAT back_buffer_fmt = DXGI_FORMAT_R8G8B8A8_UNORM;
    DXGI_FORMAT depth_stencil_fmt = DXGI_FORMAT_D24_UNORM_S8_UINT;

    WRL::ComPtr<ID3D12DescriptorHeap> p_rtv_heap;
    WRL::ComPtr<ID3D12DescriptorHeap> p_dsv_heap;
    UINT rtv_descriptor_size = 0;
    UINT dsv_descriptor_size = 0;
    UINT cbv_srv_uav_descriptor_size = 0;

    D3D12_VIEWPORT viewport;
    D3D12_RECT scissors;

    std::wstring win_caption = L"D3D12 App";
    D3D_DRIVER_TYPE d3d_drive_ty = D3D_DRIVER_TYPE_HARDWARE;

    int client_width = 960;
    int client_height = 540;
    Timer timer;
};