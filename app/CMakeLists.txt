find_package(imgui CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)

add_executable(remod-app WIN32 main.cpp browser.cpp mesh_view.cpp movie_view.cpp zoom_view.cpp)
target_link_libraries(remod-app PRIVATE
    remod_core
    imgui::imgui
    unofficial::imgui-node-editor::imgui-node-editor
    nfd::nfd
    d3d11 d3dcompiler winmm)
