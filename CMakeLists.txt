cmake_minimum_required(VERSION 3.24)
project(FF7RebirthVegetationFix VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE)
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:/permissive-;/utf-8>")
# Full warnings for our code only; the third-party hash below is built without them.
set(WARNINGS "$<$<COMPILE_LANGUAGE:CXX>:/W4>")

# DXIL container hash from the DirectX Shader Compiler (University of Illinois/NCSA license).
add_library(dxilhash STATIC third_party/dxc/DxilHash.cpp)
target_include_directories(dxilhash PUBLIC third_party/dxc)
target_compile_options(dxilhash PRIVATE /W0)

# The ReShade add-on, with a version resource for ReShade's add-on list.
configure_file(src/version.rc.in version.rc @ONLY)
add_library(FF7RebirthVegetationFix SHARED src/addon.cpp src/dxil_patcher.cpp src/pipeline_recipe.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_include_directories(FF7RebirthVegetationFix PRIVATE src third_party/reshade third_party/imgui)
target_compile_options(FF7RebirthVegetationFix PRIVATE ${WARNINGS})
target_link_libraries(FF7RebirthVegetationFix PRIVATE dxilhash)
set_target_properties(FF7RebirthVegetationFix PROPERTIES SUFFIX ".addon64")

# Offline patcher, for testing the shader patch against dumped shaders.
add_executable(dxilpatch tools/dxilpatch.cpp src/dxil_patcher.cpp)
target_include_directories(dxilpatch PRIVATE src)
target_compile_options(dxilpatch PRIVATE ${WARNINGS})
target_link_libraries(dxilpatch PRIVATE dxilhash)
