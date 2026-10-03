find_package(Catch2 3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(remod_tests test_profile.cpp test_package.cpp test_process.cpp test_texture.cpp test_graph.cpp test_settings.cpp test_image.cpp test_setup.cpp test_route.cpp test_browse.cpp test_mesh.cpp test_names.cpp test_api.cpp test_run_program.cpp test_custom.cpp test_mcp.cpp test_conditions.cpp)
target_link_libraries(remod_tests PRIVATE remod_core Catch2::Catch2WithMain nlohmann_json::nlohmann_json shell32)
target_compile_definitions(remod_tests PRIVATE
    REMOD_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles"
    REMOD_SCHEMAS_DIR="${PROJECT_SOURCE_DIR}/schemas"
    REMOD_BLOCKS_DIR="${PROJECT_SOURCE_DIR}/blocks"
    REMOD_EXAMPLES_DIR="${PROJECT_SOURCE_DIR}/examples")

include(Catch)
catch_discover_tests(remod_tests)
