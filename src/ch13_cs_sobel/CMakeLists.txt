add_executable(ch13_cs_sobel
    main.cpp
    FrameResource.cpp
    Wave.cpp
    RenderTarget.cpp
    SobelFilter.cpp
)

target_link_libraries(ch13_cs_sobel
    PUBLIC d3d_common DirectXTK12
)