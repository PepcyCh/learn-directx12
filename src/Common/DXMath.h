#pragma once

#include <random>

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

    static int RandI(int l, int r) {
        if (!rnd_init) {
            rnd_gen.seed(rnd_dv());
            rnd_init = true;
        }
        std::uniform_int_distribution<> rnd_int(l, r);
        return rnd_int(rnd_gen);
    }
    static float RandF(float l, float r) {
        if (!rnd_init) {
            rnd_gen.seed(rnd_dv());
            rnd_init = true;
        }
        std::uniform_real_distribution<> rnd_real(l, r);
        return rnd_real(rnd_gen);
    }

    static DirectX::XMVECTOR Spherical2Cartesian(float radius, float theta, float phi) {
        return DirectX::XMVectorSet(
            radius * std::sin(phi) * std::cos(theta),
            radius * std::cos(phi),
            radius * std::sin(phi) * std::sin(theta),
            1.0f
        );
    }

  private:
    inline static bool rnd_init = false;
    inline static std::random_device rnd_dv;
    inline static std::mt19937 rnd_gen;
};