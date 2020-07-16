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