cmake_minimum_required(VERSION 3.24)
project(FF7RebirthHairFix VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options(/W4 /permissive- /utf-8)

# The ReShade add-on.
# DXIL container hash from the DirectX Shader Compiler (University of Illinois/NCSA license).
add_library(dxilhash STATIC third_party/dxc/DxilHash.cpp)
target_include_directories(dxilhash PUBLIC third_party/dxc)
target_compile_options(dxilhash PRIVATE /W0)

add_library(FF7RebirthHairFix SHARED src/addon.cpp src/dxil_patcher.cpp src/pipeline_recipe.cpp)
target_include_directories(FF7RebirthHairFix PRIVATE src third_party/reshade third_party/imgui)
target_link_libraries(FF7RebirthHairFix PRIVATE dxilhash)
set_target_properties(FF7RebirthHairFix PROPERTIES SUFFIX ".addon64")

# Offline patcher, for testing the shader patch against dumped shaders.
add_executable(dxilpatch tools/dxilpatch.cpp src/dxil_patcher.cpp)
target_include_directories(dxilpatch PRIVATE src)
target_link_libraries(dxilpatch PRIVATE dxilhash)
