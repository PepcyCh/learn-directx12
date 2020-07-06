#include <iostream>

#include <DirectXColors.h>

#include "D3DApp.h"
#include "D3DUtil.h"
#include "d3dx12.h"

class D3DAppInit : public D3DApp {
  public:
    D3DAppInit(HINSTANCE h_inst) : D3DApp(h_inst) {}
    ~D3DAppInit() {}

    // bool Initialize() override;

  private:
    // void OnResize() override;
    void Update(const Timer &timer) override {}
    void Draw(const Timer &timer) override {
        // reset
        ThrowIfFailed(p_cmd_allocator->Reset());
        ThrowIfFailed(p_cmd_list->Reset(p_cmd_allocator.Get(), nullptr));

        // transit back buffer state (present -> render target)
        p_cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

        // viewport & scissors
        p_cmd_list->RSSetViewports(1, &viewport);
        p_cmd_list->RSSetScissorRects(1, &scissors);

        // clear back buffer and depth/stencil buffer.
        p_cmd_list->ClearRenderTargetView(CurrBackBufferView(), DirectX::Colors::LightSteelBlue, 0, nullptr);
        p_cmd_list->ClearDepthStencilView(DepthStencilView(),
            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
        
        // set render target
        p_cmd_list->OMSetRenderTargets(1, &CurrBackBufferView(), true, &DepthStencilView());
        
        // transit back buffer state (render target -> present)
        p_cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrBackBuffer(),
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

        ThrowIfFailed(p_cmd_list->Close());
    
        // Add the command list to the queue for execution.
        ID3D12CommandList *cmds[] = { p_cmd_list.Get() };
        p_cmd_queue->ExecuteCommandLists(sizeof(cmds) / sizeof(cmds[0]), cmds);
        
        // swap buffers
        ThrowIfFailed(p_swap_chain->Present(0, 0));
        curr_back_buffer = (curr_back_buffer + 1) % kSwapChainBufferCnt;

        // flush
        FlushCommandQueue();
    }
};

int WINAPI WinMain(HINSTANCE h_inst, HINSTANCE h_prev_inst, PSTR cmd_line, int show_cmd) {
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try {
        D3DAppInit d3d_app(h_inst);
        if (!d3d_app.Initialize()) {
            return 0;
        }
        return d3d_app.Run();
    } catch (DxException &e) {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}