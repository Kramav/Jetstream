find_package(imgui CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)

add_executable(remod-app WIN32 main.cpp browser.cpp cutscene_view.cpp mesh_view.cpp movie_view.cpp zoom_view.cpp)
target_link_libraries(remod-app PRIVATE
    remod_core
    imgui::imgui
    unofficial::imgui-node-editor::imgui-node-editor
    nfd::nfd
    d3d11 d3dcompiler winmm)

# The Cutscene layout drawn and used headless (no window, no renderer): cutscene_view_smoke.cpp.
add_executable(cutscene_view_smoke cutscene_view_smoke.cpp cutscene_view.cpp)
target_link_libraries(cutscene_view_smoke PRIVATE remod_core imgui::imgui)
add_test(NAME cutscene_view_smoke COMMAND cutscene_view_smoke ${CMAKE_SOURCE_DIR}/spikes/actor_test.json)
