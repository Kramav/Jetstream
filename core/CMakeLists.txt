# No UI dependencies allowed here (CLAUDE.md §1).
find_package(tomlplusplus CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(directxtex CONFIG REQUIRED)

add_library(remod_core STATIC profile.cpp package.cpp process.cpp texture_converter.cpp graph.cpp nodes.cpp settings.cpp image.cpp setup.cpp route.cpp browse.cpp mesh.cpp names.cpp api.cpp custom.cpp)
target_include_directories(remod_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(remod_core PRIVATE tomlplusplus::tomlplusplus nlohmann_json::nlohmann_json Microsoft::DirectXTex d3d11 windowscodecs ole32 shell32)
