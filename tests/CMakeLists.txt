find_package(Catch2 3 CONFIG REQUIRED)

add_executable(remod_tests test_profile.cpp test_package.cpp test_process.cpp test_texture.cpp test_graph.cpp test_settings.cpp test_image.cpp test_setup.cpp test_route.cpp test_browse.cpp test_mesh.cpp test_names.cpp test_api.cpp)
target_link_libraries(remod_tests PRIVATE remod_core Catch2::Catch2WithMain shell32)
target_compile_definitions(remod_tests PRIVATE
    REMOD_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles"
    REMOD_SCHEMAS_DIR="${PROJECT_SOURCE_DIR}/schemas")

include(Catch)
catch_discover_tests(remod_tests)
