add_executable(ch10_blending
    main.cpp
    FrameResource.cpp
    Wave.cpp
)

target_link_libraries(ch10_blending
    PUBLIC d3d_common DirectXTK12
)