add_executable(ch14_tessellation_bezier
    main.cpp
    FrameResource.cpp
)

target_link_libraries(ch14_tessellation_bezier
    PUBLIC d3d_common DirectXTK12
)