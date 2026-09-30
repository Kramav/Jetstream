find_package(imgui CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

add_executable(remod-app WIN32 main.cpp)
target_link_libraries(remod-app PRIVATE
    imgui::imgui
    unofficial::imgui-node-editor::imgui-node-editor
    d3d11 d3dcompiler)
