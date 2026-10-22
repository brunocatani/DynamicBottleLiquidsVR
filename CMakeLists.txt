cmake_minimum_required(VERSION 4.2)
# Fallout 4 VR build. The flat Fallout 4 build stays in xmake.lua; both builds share
# native/src and shaders/, and each runtime keeps its engine glue in its own folder.
project(DynamicBottleLiquids VERSION 1.0.0 LANGUAGES CXX)

set(COMMON_LIB_F4VR_PATH "" CACHE PATH "CommonLibF4VR source checkout")
if(NOT EXISTS "${COMMON_LIB_F4VR_PATH}/CommonLibF4/include/F4SE/F4SE.h")
  message(FATAL_ERROR "Set COMMON_LIB_F4VR_PATH to the local CommonLibF4VR checkout")
endif()
set(ENABLE_FALLOUT_F4 OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_NG OFF CACHE BOOL "" FORCE)
set(ENABLE_FALLOUT_VR ON CACHE BOOL "" FORCE)
set(F4SE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${COMMON_LIB_F4VR_PATH}/CommonLibF4" CommonLibF4 EXCLUDE_FROM_ALL)
set_property(TARGET CommonLibF4 PROPERTY INTERPROCEDURAL_OPTIMIZATION OFF)
target_compile_options(CommonLibF4 PRIVATE /wd4715)
find_package(minhook CONFIG REQUIRED)

set(SHARED_SOURCES
  native/src/BottleFrame.cpp
  native/src/NativeOptics.cpp
  native/src/Profiles.cpp
  native/src/RegionComposer.cpp
  native/src/Renderer.cpp
  native/src/Simulation.cpp)

configure_file(cmake/version.rc.in version.rc @ONLY)
add_library(DynamicBottleLiquids SHARED
  ${SHARED_SOURCES}
  vr/src/VrHooks.cpp
  vr/src/VrPlugin.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/version.rc")
target_compile_features(DynamicBottleLiquids PRIVATE cxx_std_23)
target_compile_definitions(DynamicBottleLiquids PRIVATE DBL_FALLOUT_VR WIN32_LEAN_AND_MEAN NOMINMAX NOMMNOSOUND UNICODE _UNICODE)
target_compile_options(DynamicBottleLiquids PRIVATE /W4 /wd4715 /utf-8 /Zi /permissive- /bigobj)
target_link_options(DynamicBottleLiquids PRIVATE /DEBUG:FULL)
target_include_directories(DynamicBottleLiquids PRIVATE vr/src native/src)
target_link_libraries(DynamicBottleLiquids PRIVATE CommonLibF4::CommonLibF4 minhook::minhook
  d3d11.lib d3dcompiler.lib dxgi.lib shell32.lib)
target_precompile_headers(DynamicBottleLiquids PRIVATE native/src/PCH.h)

# Shaders: the same sources as the flat build, compiled with the Fallout 4 VR stereo
# defines (instanced per-eye producer, packed side-by-side composition).
find_program(FXC_EXECUTABLE fxc
  PATHS "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
  REQUIRED)
set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(GLOB SHADER_INCLUDES CONFIGURE_DEPENDS shaders/*.hlsli shaders/*.inc)
set(SHADER_BINARIES)
function(dbl_shader source stage entry output)
  set(binary "${SHADER_OUTPUT_DIR}/${output}.cso")
  set(defines)
  foreach(define IN LISTS ARGN)
    list(APPEND defines /D ${define})
  endforeach()
  add_custom_command(OUTPUT "${binary}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
    COMMAND "${FXC_EXECUTABLE}" /nologo /WX /O3 /T ${stage}_5_0 /E ${entry} ${defines}
      /Fo "${binary}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}.hlsl"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${source}.hlsl" ${SHADER_INCLUDES}
    COMMENT "Compiling shader ${output}" VERBATIM)
  set(SHADER_BINARIES ${SHADER_BINARIES} "${binary}" PARENT_SCOPE)
endfunction()
dbl_shader(OpticalParts vs VSMain OpticalParts.vs STEREO_INSTANCED=1)
dbl_shader(OpticalParts ps PSMain OpticalParts.ps STEREO_INSTANCED=1)
dbl_shader(OpticalParts ps PSMain OpticalParts.direct.ps STEREO_INSTANCED=1 DIRECT_DISTANT=1)
dbl_shader(OpticalCompose vs VS OpticalCompose.vs STEREO_PACKED=1)
dbl_shader(OpticalCompose ps PS OpticalCompose.ps STEREO_PACKED=1)
dbl_shader(OpticalCompose ps PS OpticalCompose.scaled.ps STEREO_PACKED=1 UPSCALED_VIEWPORT=1)
dbl_shader(OpticalCompose cs CS OpticalCompose.region.cs STEREO_PACKED=1 REGION_COMPOSE=1)
dbl_shader(OpticalCompose cs CS OpticalCompose.region-scaled.cs STEREO_PACKED=1 REGION_COMPOSE=1 UPSCALED_VIEWPORT=1)
dbl_shader(RegionBounds cs CS RegionBounds.reduce.cs REDUCE=1)
dbl_shader(RegionBounds cs CS RegionBounds.finish.cs FINISH=1)
dbl_shader(RegionBounds cs CS RegionBounds.copy.cs)
add_custom_target(DynamicBottleLiquidsShaders ALL DEPENDS ${SHADER_BINARIES})
add_dependencies(DynamicBottleLiquids DynamicBottleLiquidsShaders)

# Profiles and simulation are engine-free; linking CommonLibF4VR would run its module
# initialization, which requires the game process.
add_executable(ProfileTests tests/ProfileTests.cpp native/src/Profiles.cpp native/src/Simulation.cpp)
target_compile_features(ProfileTests PRIVATE cxx_std_23)
target_compile_definitions(ProfileTests PRIVATE DBL_FALLOUT_VR DBL_ENGINE_FREE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_compile_options(ProfileTests PRIVATE /utf-8 /permissive- /bigobj)
target_include_directories(ProfileTests PRIVATE vr/src native/src)
target_precompile_headers(ProfileTests PRIVATE native/src/PCH.h)

enable_testing()
add_test(NAME ProfileTests COMMAND ProfileTests)

option(POST_BUILD_COPY_PLUGIN "Deploy the DLL, PDB and shaders" OFF)
set(COPY_PLUGIN_BASE_PATH "" CACHE PATH "MO2 mod root for this plugin")
if(POST_BUILD_COPY_PLUGIN)
  if(COPY_PLUGIN_BASE_PATH STREQUAL "")
    message(FATAL_ERROR "COPY_PLUGIN_BASE_PATH is required for deployment")
  endif()
  set(DEPLOY_PLUGINS "${COPY_PLUGIN_BASE_PATH}/F4SE/Plugins")
  add_custom_command(TARGET DynamicBottleLiquids POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DEPLOY_PLUGINS}/DynamicBottleLiquids/shaders"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:DynamicBottleLiquids>" "${DEPLOY_PLUGINS}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:DynamicBottleLiquids>" "${DEPLOY_PLUGINS}/"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${SHADER_BINARIES} "${DEPLOY_PLUGINS}/DynamicBottleLiquids/shaders/"
    COMMENT "Deploying DynamicBottleLiquids DLL, PDB and shaders")
endif()

# Tests share build-fast but stay out of the default build, so building and deploying the
# plugin never compiles them. Build them with `--target build_tests`, then run ctest.
function(fo4dev_collect_targets directory result)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${child}" NORMALIZE child_is_ours)
    if(child_is_ours)
      fo4dev_collect_targets("${child}" child_targets)
      list(APPEND targets ${child_targets})
    endif()
  endforeach()
  set(${result} ${targets} PARENT_SCOPE)
endfunction()
fo4dev_collect_targets("${CMAKE_SOURCE_DIR}" fo4dev_targets)
add_custom_target(build_tests)
foreach(fo4dev_target IN LISTS fo4dev_targets)
  get_target_property(fo4dev_target_type ${fo4dev_target} TYPE)
  string(TOLOWER "${fo4dev_target}" fo4dev_target_name)
  if(fo4dev_target_name MATCHES "test" AND fo4dev_target_type MATCHES "^(EXECUTABLE|STATIC_LIBRARY|OBJECT_LIBRARY)$")
    set_target_properties(${fo4dev_target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    if(fo4dev_target_type STREQUAL "EXECUTABLE")
      add_dependencies(build_tests ${fo4dev_target})
    endif()
  endif()
endforeach()
