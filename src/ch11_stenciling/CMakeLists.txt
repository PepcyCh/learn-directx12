add_executable(ch11_stenciling
    main.cpp
    FrameResource.cpp
)

target_link_libraries(ch11_stenciling
    PUBLIC d3d_common DirectXTK12
)