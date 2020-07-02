add_executable(${PROJECT_NAME}
    main.cpp
)

add_subdirectory(D3DCommon)

target_link_libraries(${PROJECT_NAME}
    PUBLIC d3d_common
)