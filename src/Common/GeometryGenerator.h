#pragma once

#include <vector>
#include <cstdint>

#include <DirectXMath.h>

class GeometryGenerator {
  public:
    struct Vertex {
        Vertex() = default;
        Vertex(const DirectX::XMFLOAT3 &p, const DirectX::XMFLOAT3 &n, const DirectX::XMFLOAT3 &t,
            const DirectX::XMFLOAT2 &uv) : pos(p), norm(n), tan(t), texc(uv) {}
        Vertex(float px, float py, float pz, float nx, float ny, float nz, float tx, float ty, float tz,
            float u, float v) : pos(px, py, pz), norm(nx, ny, nz), tan(nx, ny, nz), texc(u, v) {}

        DirectX::XMFLOAT3 pos;
        DirectX::XMFLOAT3 norm;
        DirectX::XMFLOAT3 tan;
        DirectX::XMFLOAT2 texc;
    };

    struct MeshData {
        std::vector<uint16_t> &GetIndices16() {
            if (indices16.empty()) {
                indices16.resize(indices32.size());
                for (int i = 0; i < indices32.size(); i++) {
                    indices16[i] = indices32[i];
                }
            }
            return indices16;
        }
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices32;

      private:
        std::vector<uint16_t> indices16;
    };

    MeshData Box(float w, float h, float d, int n_subdiv);
    MeshData Sphere(float radius, int n_slice, int n_stack);
    MeshData Geosphere(float radius, int n_subdiv);
    MeshData Cylinder(float bottom_radius, float top_radius, float h, int n_slice, int n_stack);
    MeshData Grid(float w, float d, int n, int m);
    MeshData Quad(float x, float y, float w, float h, float d);

  private:
    void Subdivide(MeshData &mesh);
    Vertex Midpoint(const Vertex &v0, const Vertex &v1);
    void BuildCylinderBottomCap(float br, float tr, float h, int n_slice, int n_stack, MeshData &mesh);
    void BuildCylinderTopCap(float br, float tr, float h, int n_slice, int n_stack, MeshData &mesh);
};