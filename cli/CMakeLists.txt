add_executable(cpp-dds main.cpp)

target_link_libraries(cpp-dds PRIVATE cppdds_lib)

target_include_directories(cpp-dds
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

install(TARGETS cpp-dds DESTINATION bin)
