add_executable(ch07_shape
    main.cpp
    FrameResource.cpp
)

target_link_libraries(ch07_shape
    PUBLIC d3d_common
)