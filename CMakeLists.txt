cmake_minimum_required(VERSION 3.25)
project(remod LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

enable_testing()

add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(app)
add_subdirectory(tests)
