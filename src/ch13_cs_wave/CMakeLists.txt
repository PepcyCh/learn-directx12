add_executable(ch13_cs_wave
    main.cpp
    FrameResource.cpp
    Wave.cpp
)

target_link_libraries(ch13_cs_wave
    PUBLIC d3d_common DirectXTK12
)