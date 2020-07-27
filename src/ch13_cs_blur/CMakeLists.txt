add_executable(ch13_cs_blur
    main.cpp
    FrameResource.cpp
    Wave.cpp
    BlurFilter.cpp
)

target_link_libraries(ch13_cs_blur
    PUBLIC d3d_common DirectXTK12
)