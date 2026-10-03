cmake_minimum_required(VERSION 3.25)
project(remod VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()
add_compile_definitions(REMOD_VERSION="${PROJECT_VERSION}")

enable_testing()

add_subdirectory(core)
add_subdirectory(cli)
add_subdirectory(app)
add_subdirectory(tests)

# Release zip: cmake --preset release, cmake --build build-release, then cpack in build-release.
# Flat layout: the exes find profiles\ next to them (find_profiles_dir).
install(TARGETS remod-app remod RUNTIME DESTINATION .)
# The C++ runtime is static, but DirectXTex's OpenMP (vcomp140.dll) has no static form: ship it app-local
# (allowed by the VC++ redistributable terms), so the zip runs without the redistributable installed.
set(CMAKE_INSTALL_OPENMP_LIBRARIES ON)
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP ON)
include(InstallRequiredSystemLibraries)
list(FILTER CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS INCLUDE REGEX "vcomp")
install(PROGRAMS ${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS} DESTINATION .)
install(DIRECTORY profiles DESTINATION .)
install(FILES schemas/graph.v0.example.json DESTINATION examples RENAME texture_mod.json)
install(FILES docs/USER_GUIDE.md DESTINATION . RENAME README.md)
set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_FILE_NAME "remod-${PROJECT_VERSION}-win64")
include(CPack)
