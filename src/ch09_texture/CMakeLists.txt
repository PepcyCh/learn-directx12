add_executable(ch09_texture
    main.cpp
    FrameResource.cpp
    Wave.cpp
)

target_link_libraries(ch09_texture
    PUBLIC d3d_common DirectXTK12
)