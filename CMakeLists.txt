cmake_minimum_required(VERSION 3.24)
project(FF7RebirthVegetationAndHairFix VERSION 0.3.0 LANGUAGES CXX)

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

# The add-on's own compute shaders, compiled to DXIL headers with the Windows SDK's dxc (which signs them with the
# dxil.dll next to it): shader(<symbol> <file> <profile> <entry>) writes ${SHADER_DIR}/<symbol>.h.
find_program(FXC fxc REQUIRED)
get_filename_component(SDK_BIN_DIR ${FXC} DIRECTORY)
find_program(DXC dxc HINTS ${SDK_BIN_DIR} NO_DEFAULT_PATH REQUIRED)
set(SHADER_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_HEADERS)
function(shader symbol file profile entry)
  set(header ${SHADER_DIR}/${symbol}.h)
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_DIR}
    COMMAND ${DXC} -nologo -T ${profile} -E ${entry} -O3 -HV 2021 -Vn ${symbol} -Fh ${header} ${CMAKE_SOURCE_DIR}/src/${file}
    DEPENDS src/${file}
    VERBATIM)
  set(SHADER_HEADERS ${SHADER_HEADERS} ${header} PARENT_SCOPE)
endfunction()
shader(kSoftCoreColors soft_edges.hlsl cs_6_0 CoreColors)
shader(kSoftDownsample soft_edges.hlsl cs_6_0 Downsample)
shader(kSoftComposite soft_edges.hlsl cs_6_0 Composite)

# The ReShade add-on, with a version resource for ReShade's add-on list.
configure_file(src/version.rc.in version.rc @ONLY)
add_library(FF7RebirthVegetationAndHairFix SHARED src/addon.cpp src/dxil_patcher.cpp src/pipeline_recipe.cpp src/soft_edges.cpp
  ${SHADER_HEADERS} ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
target_include_directories(FF7RebirthVegetationAndHairFix PRIVATE src ${SHADER_DIR} third_party/reshade third_party/imgui)
target_compile_options(FF7RebirthVegetationAndHairFix PRIVATE ${WARNINGS})
target_link_libraries(FF7RebirthVegetationAndHairFix PRIVATE dxilhash d3d12)
set_target_properties(FF7RebirthVegetationAndHairFix PROPERTIES SUFFIX ".addon64")

# Offline patcher, for testing the shader patch against dumped shaders.
add_executable(dxilpatch tools/dxilpatch.cpp src/dxil_patcher.cpp)
target_include_directories(dxilpatch PRIVATE src)
target_compile_options(dxilpatch PRIVATE ${WARNINGS})
target_link_libraries(dxilpatch PRIVATE dxilhash)

# Offline test of the soft-edge composite with the D3D12 debug layer.
add_executable(softedges_test tools/softedges_test.cpp src/soft_edges.cpp ${SHADER_HEADERS})
target_include_directories(softedges_test PRIVATE src ${SHADER_DIR})
target_compile_options(softedges_test PRIVATE ${WARNINGS})
target_link_libraries(softedges_test PRIVATE d3d12 dxgi)
