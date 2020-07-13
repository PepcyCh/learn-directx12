add_executable(ch07_land_wave
    main.cpp
    FrameResource.cpp
    Wave.cpp
)

target_link_libraries(ch07_land_wave
    PUBLIC d3d_common
)