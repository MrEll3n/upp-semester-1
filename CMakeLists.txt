cmake_minimum_required(VERSION 3.16)
project(semestralka_1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenMP REQUIRED)

add_executable(upp_sp1
    main.cpp
    data_processor.cpp
    svg_writer.cpp
)

target_link_libraries(upp_sp1 PRIVATE OpenMP::OpenMP_CXX)
