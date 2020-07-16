#pragma once

#include <string>
#include <unordered_map>

#include <windows.h>
#include <wrl.h>
#include <comdef.h>

#include <d3d12.h>
#include <dxgi1_5.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <DirectXCollision.h>

#include "d3dx12.h"
#include "DXMath.h"

class D3DUtil {
  public:
    // size of cbuffer must be multiple of 256
    static UINT CBSize(UINT size) {
        return (size + 255) & (~255);
    }

    // load shader from cso(compiled shader object) binary file
    static Microsoft::WRL::ComPtr<ID3DBlob> LoadBinary(const std::wstring &filename);
    // compiler shader from hlsl file
    static Microsoft::WRL::ComPtr<ID3DBlob> CompileShader(const std::wstring &filename,
        const D3D_SHADER_MACRO *defines, const std::string &entry, const std::string &target);

    // create default resource that holds data using upload_buf as intermediate upload buffer
    static Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(ID3D12Device *device,
        ID3D12GraphicsCommandList *cmd_list, const void *data, UINT size,
        Microsoft::WRL::ComPtr<ID3D12Resource> &upload_buf);
};

struct SubmeshGeometry {
    UINT n_index;
    UINT start_index;
    UINT base_vertex;
    DirectX::BoundingBox bbox;
};

struct MeshGeometry {
    std::string name;

    // x_cpu holds data
    Microsoft::WRL::ComPtr<ID3DBlob> vb_cpu = nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> ib_cpu = nullptr;

    // x_gpu provide data for dx
    Microsoft::WRL::ComPtr<ID3D12Resource> vb_gpu = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> ib_gpu = nullptr;

    // uploader as intermediate buffer
    Microsoft::WRL::ComPtr<ID3D12Resource> vb_uploader = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> ib_uploader = nullptr;

    UINT vb_stride = 0; // size per vertex
    UINT vb_size = 0;
    DXGI_FORMAT index_fmt = DXGI_FORMAT_R16_UINT;
    UINT ib_size = 0;

    std::unordered_map<std::string, SubmeshGeometry> draw_args;

    D3D12_VERTEX_BUFFER_VIEW VertexBufferView() const;
    D3D12_INDEX_BUFFER_VIEW IndexBufferView() const;

    void DisposeUploader();
};

class DxException {
  public:
    DxException() = default;
    DxException(HRESULT hr, const std::wstring &fn_name,
        const std::wstring &file_name, int lineno) : err_code(hr),
        fn_name(fn_name), file_name(file_name), lineno(lineno) {}

    std::wstring ToString() const {
        _com_error err(err_code);
        std::wstring msg = err.ErrorMessage();
        return fn_name + L" failed in " + file_name + L"; line " + std::to_wstring(lineno) + L"; error: " + msg;
    }

    HRESULT err_code = S_OK;
    std::wstring fn_name;
    std::wstring file_name;
    int lineno = -1;
};

// hlsl bind 4 continues float to a float4 (in a register)
// and data in the same vector won't be splitted
// { float3, float, float3, float, float3, float } - 3 float4
// { float3, float3, float3, float, float, float } - 4 float4
//
// falloff: instead of using 1 / d^2 as attenuation, use linear function between falloff_start and falloff_end:

// directional/point/spot light
struct Light {
    DirectX::XMFLOAT3 strength = { 0.5f, 0.5f, 0.5f };
    float falloff_start = 1.0f; // point/spot
    DirectX::XMFLOAT3 direction = { 0.0f, -1.0f, 0.0f }; // directional/spot
    float falloff_end = 10.0f; // point/spot
    DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f }; // point/spot
    float spot_power = 64.0f; // spot
};

const int MAX_N_LIGHT = 16;

struct MaterialConst {
    DirectX::XMFLOAT4 albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 fresnel_r0 = { 0.01f, 0.01f, 0.01f };
    float roughness = 0.25f;
    DirectX::XMFLOAT4X4 mat_transform = DXMath::Identity4x4();
};

struct Material {
    std::string name;
    int mat_cb_ind = -1;
    int diffuse_srv_heap_index = -1;
    int normal_srv_heap_index = -1;
    int n_frame_dirty = 0;
    DirectX::XMFLOAT4 albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
    DirectX::XMFLOAT3 fresnel_r0 = { 0.01f, 0.01f, 0.01f };
    float roughness = 0.25f;
    DirectX::XMFLOAT4X4 mat_transform = DXMath::Identity4x4();
};

inline std::wstring AnsiToWString(const std::string &str) {
    WCHAR buffer[512];
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, buffer, 512);
    return std::wstring(buffer);
}

#ifndef ThrowIfFailed
#define ThrowIfFailed(x) do {\
    HRESULT _hr = (x); \
    std::wstring file = AnsiToWString(__FILE__); \
    if (FAILED(_hr)) { \
        throw DxException(_hr, L#x, file, __LINE__); \
    } \
} while(0)
#endif