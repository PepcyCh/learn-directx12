configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/defines.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/defines.h
)

add_subdirectory(Common)

add_subdirectory(ch04_init)
add_subdirectory(ch06_box)
add_subdirectory(ch06_box_extra)
add_subdirectory(ch07_shape)
add_subdirectory(ch07_land_wave)
add_subdirectory(ch08_lighting)
add_subdirectory(ch09_texture)
add_subdirectory(ch10_blending)
add_subdirectory(ch11_stenciling)
add_subdirectory(ch12_gs)
add_subdirectory(ch13_cs_wave)
add_subdirectory(ch13_cs_blur)
add_subdirectory(ch13_cs_sobel)
add_subdirectory(ch14_tessellation_basic)