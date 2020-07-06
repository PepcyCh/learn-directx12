#pragma once

#include <DirectXMath.h>
#include <DirectXPackedVector.h>

class DXMath {
  public:
    static DirectX::XMFLOAT4X4 Identity4x4() {
        static DirectX::XMFLOAT4X4 eye(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f
        );
        return eye;
    }
};