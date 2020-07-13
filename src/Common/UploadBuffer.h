#pragma once

#include "D3DUtil.h"

template <typename T>
class UploadBuffer {
  public:
    UploadBuffer(ID3D12Device *device, UINT n_ele, bool is_const) : is_const(is_const) {
        ele_size = sizeof(T);
        if (is_const) {
            ele_size = D3DUtil::CBSize(ele_size);
        }

        ThrowIfFailed(device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE, &CD3DX12_RESOURCE_DESC::Buffer(n_ele * ele_size),
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_buf)));
        ThrowIfFailed(upload_buf->Map(0, nullptr, reinterpret_cast<void **>(&mapped_data)));
    }

    UploadBuffer(const UploadBuffer &rhs) = delete;
    UploadBuffer &operator=(const UploadBuffer &rhs) = delete;

    ~UploadBuffer() {
        if (upload_buf != nullptr) {
            upload_buf->Unmap(0, nullptr);
        }
        upload_buf = nullptr;
    }

    ID3D12Resource *Resource() const {
        return upload_buf.Get();
    }

    void CopyData(int i_ele, const T &data) {
        memcpy(mapped_data + i_ele * ele_size, &data, sizeof(T));
    }

  private:
    Microsoft::WRL::ComPtr<ID3D12Resource> upload_buf;
    BYTE *mapped_data;
    UINT ele_size;
    bool is_const;
};