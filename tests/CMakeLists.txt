find_package(Catch2 3 CONFIG REQUIRED)

add_executable(remod_tests test_profile.cpp test_package.cpp test_process.cpp)
target_link_libraries(remod_tests PRIVATE remod_core Catch2::Catch2WithMain shell32)
target_compile_definitions(remod_tests PRIVATE REMOD_PROFILES_DIR="${PROJECT_SOURCE_DIR}/profiles")

include(Catch)
catch_discover_tests(remod_tests)
