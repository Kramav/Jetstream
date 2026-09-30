# No UI dependencies allowed here (CLAUDE.md §1).
find_package(tomlplusplus CONFIG REQUIRED)

add_library(remod_core STATIC profile.cpp package.cpp process.cpp)
target_include_directories(remod_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(remod_core PRIVATE tomlplusplus::tomlplusplus)
