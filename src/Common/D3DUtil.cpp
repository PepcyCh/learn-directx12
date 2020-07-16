#include "D3DUtil.h"

#include <fstream>

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

ComPtr<ID3DBlob> D3DUtil::LoadBinary(const std::wstring &filename) {
    std::ifstream fin(filename, std::ios::binary);

    fin.seekg(0, std::ios::end);
    auto file_size = fin.tellg();
    fin.seekg(0, std::ios::beg);

    ComPtr<ID3DBlob> blob;
    ThrowIfFailed(D3DCreateBlob(file_size, &blob));

    fin.read((char *) blob->GetBufferPointer(), file_size);
    fin.close();

    return blob;
}

ComPtr<ID3DBlob> D3DUtil::CompileShader(const std::wstring &filename,
        const D3D_SHADER_MACRO *defines, const std::string &entry,
        const std::string &target) {
    UINT compile_flag = 0;
#if defined(DEBUG) || defined(_DEBUG)
    compile_flag = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    HRESULT hr = S_OK;

    ComPtr<ID3DBlob> code = nullptr;
    ComPtr<ID3DBlob> errors;

    hr = D3DCompileFromFile(filename.c_str(), defines, D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entry.c_str(), target.c_str(), compile_flag, 0, &code, &errors);

    if (errors != nullptr) {
        OutputDebugStringA((char *) errors->GetBufferPointer());
    }

    ThrowIfFailed(hr);

    return code;
}

ComPtr<ID3D12Resource> D3DUtil::CreateDefaultBuffer(ID3D12Device *device,
        ID3D12GraphicsCommandList *cmd_list, const void *data, UINT size,
        ComPtr<ID3D12Resource> &upload_buf) {
    ComPtr<ID3D12Resource> default_buf;

    // create default resource
    ThrowIfFailed(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(size), D3D12_RESOURCE_STATE_COMMON,
        nullptr, IID_PPV_ARGS(&default_buf)));

    // create upload buffer
    ThrowIfFailed(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(size), D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&upload_buf)));

    // copy data
    // intermediate buffer map data and copy them to destination resource
    D3D12_SUBRESOURCE_DATA sub_rsc_data = {};
    sub_rsc_data.pData = data;
    sub_rsc_data.RowPitch = size;
    sub_rsc_data.SlicePitch = sub_rsc_data.RowPitch;
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(default_buf.Get(),
        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
    UpdateSubresources<1>(cmd_list, default_buf.Get(), upload_buf.Get(), 0, 0, 1, &sub_rsc_data);
    cmd_list->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(default_buf.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_GENERIC_READ));

    return default_buf;
}

D3D12_VERTEX_BUFFER_VIEW MeshGeometry::VertexBufferView() const {
    D3D12_VERTEX_BUFFER_VIEW vbv;
    vbv.BufferLocation = vb_gpu->GetGPUVirtualAddress();
    vbv.SizeInBytes = vb_size;
    vbv.StrideInBytes = vb_stride;
    return vbv;
}

D3D12_INDEX_BUFFER_VIEW MeshGeometry::IndexBufferView() const {
    D3D12_INDEX_BUFFER_VIEW ibv;
    ibv.BufferLocation = ib_gpu->GetGPUVirtualAddress();
    ibv.Format = index_fmt;
    ibv.SizeInBytes = ib_size;
    return ibv;
}

void MeshGeometry::DisposeUploader() {
    vb_uploader = nullptr;
    ib_uploader = nullptr;
}