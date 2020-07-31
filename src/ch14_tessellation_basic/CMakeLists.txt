add_executable(ch14_tessellation_baisc
    main.cpp
    FrameResource.cpp
)

target_link_libraries(ch14_tessellation_baisc
    PUBLIC d3d_common DirectXTK12
)