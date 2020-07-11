add_library(d3d_common
    D3DApp.cpp
    D3DUtil.cpp
    GeometryGenerator.cpp
    Timer.cpp
)

target_include_directories(d3d_common
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(d3d_common
    PUBLIC d3dcompiler d3d12 dxgi
)

target_compile_definitions(d3d_common
    PUBLIC UNICODE
)