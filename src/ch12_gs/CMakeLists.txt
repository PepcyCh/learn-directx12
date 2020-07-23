add_executable(ch12_gs
    main.cpp
    FrameResource.cpp
    Wave.cpp
)

target_link_libraries(ch12_gs
    PUBLIC d3d_common DirectXTK12
)