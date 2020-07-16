add_executable(ch08_lighting
    main.cpp
    FrameResource.cpp
    Wave.cpp
)

target_link_libraries(ch08_lighting
    PUBLIC d3d_common
)